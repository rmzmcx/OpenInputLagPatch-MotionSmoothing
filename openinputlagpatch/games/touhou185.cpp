// Touhou 18.5: 100th Black Market v1.00a

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "patch_util.h"
#include "limiter.h"
#include "d3d9_hook.h"
#include "config.h"
#include "overlay.h"
#include "tool_state.h"
#include "touhou185.h"

using namespace Touhou185;

int __fastcall th185_window_update_hook(void* self) {
	Limiter::Tick();
	return CWindowManager__UpdateFast(self);
}

// The two frame functions at 0x475290 and 0x475480 both run the update chain, begin a scene,
// reset and flush the sprite queue around the render chain, end the scene and present. Only the
// pacing around that differs. These are the calls those two functions share.
static auto th185_reset_sprite_queue = (void(*)())0x00480DC0;
static auto th185_draw_sprite_queue = (void(__fastcall*)(void*))0x00480E10;
static auto th185_render_objects = (void(*)())0x00401410;

static void** const th185_sprite_manager = (void**)0x00529E88;
static DWORD* const th185_render_flag = (DWORD*)0x004D5DE8;
static const size_t th185_sprite_queue = 0xD8; // two floats in the sprite manager
static float th185_sprite_queue_saved[2];

static D3DVIEWPORT9 th185_scene_viewport = {};
static D3DMATRIX th185_scene_view = {};
static D3DMATRIX th185_scene_projection = {};
static bool th185_scene_transforms;

// thprac builds and draws its frame at these two return addresses, as it does on th18.
static unsigned char* const th185_tool_frame_address = (unsigned char*)0x004013DD;
static unsigned char* const th185_tool_draw_address = (unsigned char*)0x004014FA;

// The ASCII manager's records start at +0xC24 and its per-frame record count is at +0x19224.
// Drawing a menu string appends a record. Replaying the render walk without putting the count back
// would draw the game's own text and then another copy from the extra pass.
static void** const th185_text_object = (void**)0x004D7AD0;
static const size_t th185_text_record_count = 0x19224;
static unsigned int th185_text_records;

static unsigned int th185_text_count() {
	void* text = *th185_text_object;
	return text != nullptr ? *(unsigned int*)((char*)text + th185_text_record_count) : 0;
}

static void th185_text_records_save() {
	th185_text_records = th185_text_count();
}

static void th185_text_records_restore() {
	void* text = *th185_text_object;
	if (text != nullptr)
		*(unsigned int*)((char*)text + th185_text_record_count) = th185_text_records;
}

// The stage object's two draw callbacks decrement the stage-clear timer and can create the fade
// object. Reproducing the render chain would otherwise do that two or three times per game frame.
// Only the fields those callbacks touch are put back after each extra pass.
static void** const th185_stage_slots[2] = { (void**)0x004D7AD8, (void**)0x004D7AD4 };
static const size_t th185_stage_ranges[][2] = {
	{ 0x32C, 0x08 },
	{ 0x344C, 0x04 },
	{ 0x3468, 0x38 },
};
static const size_t th185_stage_saved_size = 0x44;
static unsigned char* th185_stage_saved[2];
static bool th185_stage_saved_valid[2];

static void th185_stage_save() {
	for (int i = 0; i < 2; ++i) {
		th185_stage_saved_valid[i] = false;
		void* stage = *th185_stage_slots[i];
		if (stage == nullptr)
			continue;
		if (th185_stage_saved[i] == nullptr)
			th185_stage_saved[i] = (unsigned char*)malloc(th185_stage_saved_size);
		if (th185_stage_saved[i] == nullptr)
			continue;
		size_t offset = 0;
		for (size_t r = 0; r < sizeof(th185_stage_ranges) / sizeof(th185_stage_ranges[0]); ++r) {
			memcpy(th185_stage_saved[i] + offset, (char*)stage + th185_stage_ranges[r][0],
				th185_stage_ranges[r][1]);
			offset += th185_stage_ranges[r][1];
		}
		th185_stage_saved_valid[i] = true;
	}
}

static void th185_stage_restore() {
	for (int i = 0; i < 2; ++i) {
		void* stage = *th185_stage_slots[i];
		if (!th185_stage_saved_valid[i] || stage == nullptr)
			continue;
		size_t offset = 0;
		for (size_t r = 0; r < sizeof(th185_stage_ranges) / sizeof(th185_stage_ranges[0]); ++r) {
			memcpy((char*)stage + th185_stage_ranges[r][0], th185_stage_saved[i] + offset,
				th185_stage_ranges[r][1]);
			offset += th185_stage_ranges[r][1];
		}
		th185_stage_saved_valid[i] = false;
	}
}

static bool th185_in_extra;

static __declspec(noinline) bool th185_is_extra() {
	return th185_in_extra;
}

// 0x425008 branches around the fade creation when the stage timer has reached 30. During an extra
// render it always takes the branch, without creating another object or registering more tasks.
static void __declspec(naked) th185_fade_create_branch() {
	__asm {
		push eax
		push ecx
		push edx
		call th185_is_extra
		test al, al
		pop edx
		pop ecx
		pop eax
		jne skip
		cmp dword ptr [edi + 347Ch], 1Eh
		jge skip
		mov ecx, 0042500Eh
		jmp ecx
	skip:
		mov ecx, 00425139h
		jmp ecx
	}
}

static void th185_render_hook() {
	IDirect3DDevice9* device = d3d9_hooked_device();
	if (device != nullptr) {
		device->GetViewport(&th185_scene_viewport);
		th185_scene_transforms =
			SUCCEEDED(device->GetTransform(D3DTS_VIEW, &th185_scene_view)) &&
			SUCCEEDED(device->GetTransform(D3DTS_PROJECTION, &th185_scene_projection));
	}

	void* sprites = *th185_sprite_manager;
	if (sprites != nullptr) {
		th185_sprite_queue_saved[0] = *(float*)((char*)sprites + th185_sprite_queue);
		th185_sprite_queue_saved[1] = *(float*)((char*)sprites + th185_sprite_queue + 4);
	}

	ToolState::WatchFrameReady(th185_tool_frame_address, th185_tool_draw_address);
	th185_text_records_save();
	th185_render_objects();
}

// The game thread's update and draw flags. While either says the picture is held, an extra pass
// must leave every object where the game left it.
static void** const th185_game_thread = (void**)0x004D7B0C;
static void** const th185_shop_object = (void**)0x004D7AC4;

static bool th185_objects_held() {
	void* thread = *th185_game_thread;
	if (thread == nullptr || *th185_shop_object != nullptr)
		return true;
	const unsigned int update_flags = *(unsigned int*)((char*)thread + 0x2C);
	const unsigned int draw_flags = *(unsigned int*)((char*)thread + 0xB0);
	return (update_flags & (1 | 4 | 0x400)) != 0 || (draw_flags & 4) != 0;
}

static float th185_time_scale() {
	const float scale = *(float*)0x004D0FE0;
	return scale > 0.0f && scale <= 2.0f ? scale : 1.0f;
}

// ---------------------------------------------------------------------------
// Bullets
// ---------------------------------------------------------------------------
//
// The manager at 0x4D7AE0 owns 2001 bullets of 0xFE4 bytes starting at +0xEC. Six lists begin at
// manager+0x10, and the next node is at bullet+0x690. A bullet's position is +0x638, velocity
// +0x644, state +0xFA8 and flags +0x20. States 2 and 3 move at half speed; the flags bit 0x200
// holds a bullet and also keeps it out of the draw lists.
static void** const th185_bullet_manager = (void**)0x004D7AE0;
static const size_t th185_bullet_lists = 0x10;
static const size_t th185_bullet_columns = 6;
static const size_t th185_bullet_next = 0x690;
static const size_t th185_bullet_position = 0x638;
static const size_t th185_bullet_velocity = 0x644;
static const size_t th185_bullet_state = 0xFA8;
static const size_t th185_bullet_flags = 0x20;
static const size_t th185_bullets_saved_max = 2001;

struct Th185SavedBullet {
	void* bullet;
	float position[3];
};

static Th185SavedBullet th185_saved_bullets[th185_bullets_saved_max];
static size_t th185_saved_bullets_count;
static unsigned int th185_bullets_in_play;
static unsigned int th185_bullets_projected;

static float th185_bullet_step(unsigned short state) {
	switch (state) {
		case 1: return 1.0f;
		case 2:
		case 3: return 0.5f;
		default: return 0.0f;
	}
}

static void th185_bullets_advance(float t) {
	th185_saved_bullets_count = 0;
	th185_bullets_in_play = 0;
	th185_bullets_projected = 0;

	void* manager = *th185_bullet_manager;
	if (manager == nullptr || th185_objects_held())
		return;

	const float scale = th185_time_scale();
	for (size_t column = 0; column < th185_bullet_columns; ++column) {
		void* bullet = *(void**)((char*)manager + th185_bullet_lists + column * 4);
		for (size_t guard = 0; bullet != nullptr && guard < th185_bullets_saved_max * 2; ++guard) {
			void* next = *(void**)((char*)bullet + th185_bullet_next);
			const unsigned int flags = *(unsigned int*)((char*)bullet + th185_bullet_flags);
			++th185_bullets_in_play;
			const float factor = th185_bullet_step(*(unsigned short*)((char*)bullet + th185_bullet_state));
			if ((flags & 0x200) == 0 && factor != 0.0f &&
				th185_saved_bullets_count < th185_bullets_saved_max) {
				float* position = (float*)((char*)bullet + th185_bullet_position);
				const float* velocity = (const float*)((char*)bullet + th185_bullet_velocity);
				Th185SavedBullet& saved = th185_saved_bullets[th185_saved_bullets_count++];
				saved.bullet = bullet;
				for (int axis = 0; axis < 3; ++axis) {
					saved.position[axis] = position[axis];
					position[axis] += velocity[axis] * factor * scale * t;
				}
				++th185_bullets_projected;
			}
			bullet = next;
		}
	}
}

static void th185_bullets_undo() {
	for (size_t i = 0; i < th185_saved_bullets_count; ++i) {
		Th185SavedBullet& saved = th185_saved_bullets[i];
		float* position = (float*)((char*)saved.bullet + th185_bullet_position);
		for (int axis = 0; axis < 3; ++axis)
			position[axis] = saved.position[axis];
	}
	th185_saved_bullets_count = 0;
}

// ---------------------------------------------------------------------------
// Player, options and player shots
// ---------------------------------------------------------------------------
//
// The player state is 0x4D7C3C. The visible body is its embedded animation object at +0x14: the
// render task at 0x462AF0 copies the player's position at +0x620 into that object's +0x604. Four
// option slots start at +0x670 and twelve shot slots at +0xA70, each 0x100 bytes, with the live flag
// at +0 and two sprite ids at +0xC0/+0xC4. Their separate sprite objects use +0x5F0.
static void** const th185_player = (void**)0x004D7C3C;
static const size_t th185_player_position = 0x620;
static const size_t th185_player_effect_sprite = 0x47FB0;
static const size_t th185_player_options = 0x670;
static const size_t th185_player_option_count = 4;
static const size_t th185_player_shots = 0xA70;
static const size_t th185_player_shot_count = 12;
static const size_t th185_player_slot_size = 0x100;
static const size_t th185_player_slot_sprites = 0xC0;

// Main shots and card-driven firepower live in the ability manager at 0x4D7AB8. It owns the main
// shot sprite at +0x80, 64 companion sprites at +0x84, and 256 shot entries at +0x69C with their
// source objects in the parallel array at +0xA9C.
static void** const th185_ability_manager = (void**)0x004D7AB8;
static const size_t th185_ability_main_shot = 0x80;
static const size_t th185_ability_companions = 0x84;
static const size_t th185_ability_companion_count = 64;
static const size_t th185_ability_shots = 0x69C;
static const size_t th185_ability_shot_sources = 0xA9C;
static const size_t th185_ability_shot_count = 256;

static void** const th185_sprite_objects = (void**)0x00529E88;
static const size_t th185_object_slot_base = 0x700;
static const size_t th185_object_slot_size = 0x624;
static const size_t th185_object_in_use = 0x61C;
static const size_t th185_object_id = 0x544;
static const size_t th185_object_position = 0x5F0;
static const float th185_sprite_max_step = 48.0f;

static const size_t th185_tracked_max =
	1 + th185_player_option_count * 2 + th185_player_shot_count * 2 +
	1 + th185_ability_companion_count + th185_ability_shot_count +
	256;

struct Th185SpriteMotion {
	void* sprite;
	unsigned int frame;
	float last_x, last_y;
	float delta_x, delta_y;
};

struct Th185TrackedSprite {
	float* position;
	unsigned int kind;
	float saved[3];
};

static Th185SpriteMotion th185_motions[th185_tracked_max];
static unsigned int th185_motion_frame = 1;
static Th185TrackedSprite th185_tracked[th185_tracked_max];
static size_t th185_tracked_count;
static unsigned int th185_sprites_untracked;
static float th185_player_movement;
static unsigned int th185_main_effect_sprite;
static unsigned int th185_options_live;
static unsigned int th185_shots_live;
static unsigned int th185_ability_sprites_live;
static unsigned int th185_ability_main_id;
static unsigned int th185_ability_companions_live;
static unsigned int th185_ability_active_shots;

static const size_t th185_scan_sprite_count = 0x7FFF;
static const unsigned int th185_scan_candidates_max = 128;

struct Th185ScannedSprite {
	float last_x, last_y;
	unsigned int frame;
};

struct Th185MovingSprite {
	unsigned int id;
	float x, y;
	float dx, dy;
};

static Th185ScannedSprite th185_scanned_sprites[th185_scan_sprite_count];
static Th185MovingSprite th185_moving_sprites[th185_scan_candidates_max];
static unsigned int th185_sprite_scan_frame = 1;
static unsigned int th185_moving_sprites_count;

static const unsigned int th185_kind_body = 0;
static const unsigned int th185_kind_shot = 1;
static const unsigned int th185_kind_generic = 2;

static void* th185_sprite_object(unsigned int id) {
	const unsigned int index = id & 0x7FFF;
	if (id == 0 || index == 0x7FFF)
		return nullptr;
	void* manager = *th185_sprite_objects;
	if (manager == nullptr)
		return nullptr;
	char* object = (char*)manager + th185_object_slot_base + (size_t)index * th185_object_slot_size;
	if (*(unsigned char*)(object + th185_object_in_use) == 0 ||
		*(unsigned int*)(object + th185_object_id) != id)
		return nullptr;
	return object;
}

static Th185SpriteMotion* th185_sprite_motion(void* sprite) {
	Th185SpriteMotion* empty = nullptr;
	Th185SpriteMotion* oldest = nullptr;
	for (size_t i = 0; i < th185_tracked_max; ++i) {
		Th185SpriteMotion& motion = th185_motions[i];
		if (motion.sprite == sprite)
			return &motion;
		if (motion.sprite == nullptr) {
			if (empty == nullptr)
				empty = &motion;
			continue;
		}
		if (motion.frame == th185_motion_frame)
			continue;
		if (oldest == nullptr || motion.frame < oldest->frame)
			oldest = &motion;
	}

	Th185SpriteMotion* motion = empty != nullptr ? empty : oldest;
	if (motion == nullptr)
		return nullptr;
	motion->sprite = sprite;
	motion->frame = 0;
	motion->last_x = 0.0f;
	motion->last_y = 0.0f;
	motion->delta_x = 0.0f;
	motion->delta_y = 0.0f;
	return motion;
}

static void th185_track_sprite(unsigned int id, unsigned int kind) {
	if (th185_tracked_count >= th185_tracked_max)
		return;
	void* object = th185_sprite_object(id);
	if (object == nullptr)
		return;
	float* position = (float*)((char*)object + th185_object_position);
	for (size_t i = 0; i < th185_tracked_count; ++i) {
		if (th185_tracked[i].position == position)
			return;
	}

	Th185SpriteMotion* motion = th185_sprite_motion(position);
	if (motion == nullptr) {
		++th185_sprites_untracked;
		return;
	}

	if (motion->frame + 1 == th185_motion_frame) {
		const float dx = position[0] - motion->last_x;
		const float dy = position[1] - motion->last_y;
		const bool sane = dx > -th185_sprite_max_step && dx < th185_sprite_max_step &&
			dy > -th185_sprite_max_step && dy < th185_sprite_max_step;
		motion->delta_x = sane ? dx : 0.0f;
		motion->delta_y = sane ? dy : 0.0f;
	} else {
		motion->delta_x = 0.0f;
		motion->delta_y = 0.0f;
	}
	motion->frame = th185_motion_frame;
	motion->last_x = position[0];
	motion->last_y = position[1];

	Th185TrackedSprite& tracked = th185_tracked[th185_tracked_count++];
	tracked.position = position;
	tracked.kind = kind;
}

static void th185_track_slot_sprites(const char* base, size_t count, unsigned int kind) {
	for (size_t i = 0; i < count; ++i) {
		const char* slot = base + i * th185_player_slot_size;
		if (*(const int*)slot == 0)
			continue;
		th185_track_sprite(*(const unsigned int*)(slot + th185_player_slot_sprites), kind);
		th185_track_sprite(*(const unsigned int*)(slot + th185_player_slot_sprites + 4), kind);
	}
}

static unsigned int th185_count_live_slots(const char* base, size_t count) {
	unsigned int live = 0;
	for (size_t i = 0; i < count; ++i) {
		if (*(const int*)(base + i * th185_player_slot_size) != 0)
			++live;
	}
	return live;
}

static void th185_track_ability_sprites() {
	void* manager = *th185_ability_manager;
	th185_ability_sprites_live = 0;
	th185_ability_main_id = 0;
	th185_ability_companions_live = 0;
	th185_ability_active_shots = 0;
	if (manager == nullptr)
		return;

	const unsigned int main_shot = *(const unsigned int*)((char*)manager + th185_ability_main_shot);
	th185_ability_main_id = main_shot;
	if (main_shot != 0) {
		th185_track_sprite(main_shot, th185_kind_shot);
		++th185_ability_sprites_live;
	}

	for (size_t i = 0; i < th185_ability_companion_count; ++i) {
		const unsigned int id =
			*(const unsigned int*)((char*)manager + th185_ability_companions + i * 4);
		if (id != 0) {
			th185_track_sprite(id, th185_kind_shot);
			++th185_ability_sprites_live;
			++th185_ability_companions_live;
		}
	}

	for (size_t i = 0; i < th185_ability_shot_count; ++i) {
		const unsigned int id =
			*(const unsigned int*)((char*)manager + th185_ability_shots + i * 4);
		const void* source =
			*(void* const*)((char*)manager + th185_ability_shot_sources + i * 4);
		if (id != 0 && source != nullptr) {
			th185_track_sprite(id, th185_kind_shot);
			++th185_ability_sprites_live;
			++th185_ability_active_shots;
		}
	}
}

// Diagnostic only: record every in-use sprite object whose drawn position moved this game frame.
// This finds the actual player-shot sprites without assuming which manager owns them.
static void th185_scan_moving_sprites() {
	++th185_sprite_scan_frame;
	th185_moving_sprites_count = 0;
	void* manager = *th185_sprite_objects;
	if (manager == nullptr)
		return;

	for (size_t index = 1; index < th185_scan_sprite_count; ++index) {
		char* object = (char*)manager + th185_object_slot_base + index * th185_object_slot_size;
		if (*(unsigned char*)(object + th185_object_in_use) == 0)
			continue;
		const unsigned int id = *(const unsigned int*)(object + th185_object_id);
		if (id == 0 || (id & 0x7FFF) != index)
			continue;
		const float* position = (const float*)(object + th185_object_position);
		Th185ScannedSprite& scanned = th185_scanned_sprites[index];
		if (scanned.frame + 1 == th185_sprite_scan_frame) {
			const float dx = position[0] - scanned.last_x;
			const float dy = position[1] - scanned.last_y;
			const float adx = dx < 0.0f ? -dx : dx;
			const float ady = dy < 0.0f ? -dy : dy;
			if ((adx > 0.05f || ady > 0.05f) && adx < 48.0f && ady < 48.0f &&
				th185_moving_sprites_count < th185_scan_candidates_max) {
				Th185MovingSprite& moving =
					th185_moving_sprites[th185_moving_sprites_count++];
				moving.id = id;
				moving.x = position[0];
				moving.y = position[1];
				moving.dx = dx;
				moving.dy = dy;
			}
		}
		scanned.frame = th185_sprite_scan_frame;
		scanned.last_x = position[0];
		scanned.last_y = position[1];
	}
}

static void th185_track_scanned_sprites() {
	for (unsigned int i = 0; i < th185_moving_sprites_count; ++i)
		th185_track_sprite(th185_moving_sprites[i].id, th185_kind_generic);
}

static void th185_player_measure() {
	th185_tracked_count = 0;
	th185_sprites_untracked = 0;
	++th185_motion_frame;

	void* player = *th185_player;
	if (player == nullptr)
		return;

	th185_main_effect_sprite = *(const unsigned int*)((char*)player + th185_player_effect_sprite);
	th185_options_live = th185_count_live_slots(
		(const char*)player + th185_player_options, th185_player_option_count);
	th185_shots_live = th185_count_live_slots(
		(const char*)player + th185_player_shots, th185_player_shot_count);

	float* body_position = (float*)((char*)player + th185_player_position);
	Th185SpriteMotion* body_motion = th185_sprite_motion(body_position);
	if (body_motion != nullptr) {
		if (body_motion->frame + 1 == th185_motion_frame) {
			const float dx = body_position[0] - body_motion->last_x;
			const float dy = body_position[1] - body_motion->last_y;
			const bool sane = dx > -th185_sprite_max_step && dx < th185_sprite_max_step &&
				dy > -th185_sprite_max_step && dy < th185_sprite_max_step;
			body_motion->delta_x = sane ? dx : 0.0f;
			body_motion->delta_y = sane ? dy : 0.0f;
		} else {
			body_motion->delta_x = 0.0f;
			body_motion->delta_y = 0.0f;
		}
		body_motion->frame = th185_motion_frame;
		body_motion->last_x = body_position[0];
		body_motion->last_y = body_position[1];
	}
	if (th185_tracked_count < th185_tracked_max) {
		Th185TrackedSprite& tracked = th185_tracked[th185_tracked_count++];
		tracked.position = body_position;
		tracked.kind = th185_kind_body;
	}
	if (th185_tracked_count != 0) {
		const Th185SpriteMotion* motion = th185_sprite_motion(th185_tracked[0].position);
		if (motion != nullptr) {
			const float x = motion->delta_x < 0.0f ? -motion->delta_x : motion->delta_x;
			const float y = motion->delta_y < 0.0f ? -motion->delta_y : motion->delta_y;
			if (x > th185_player_movement)
				th185_player_movement = x;
			if (y > th185_player_movement)
				th185_player_movement = y;
		}
	}

	th185_track_slot_sprites((const char*)player + th185_player_options,
		th185_player_option_count, th185_kind_body);
	th185_track_slot_sprites((const char*)player + th185_player_shots,
		th185_player_shot_count, th185_kind_shot);
	th185_track_ability_sprites();
	th185_scan_moving_sprites();
	th185_track_scanned_sprites();
}

static void th185_player_advance(float t) {
	for (size_t i = 0; i < th185_tracked_count; ++i) {
		Th185TrackedSprite& tracked = th185_tracked[i];
		for (int axis = 0; axis < 3; ++axis)
			tracked.saved[axis] = tracked.position[axis];
		const Th185SpriteMotion* motion = th185_sprite_motion(tracked.position);
		const bool move = tracked.kind == th185_kind_shot
			? Config::InterpolatePlayerShots
			: tracked.kind == th185_kind_body
				? Config::InterpolatePlayerBody
				: Config::InterpolatePlayerShots || Config::InterpolateBulletsAndLasers;
		if (move && motion != nullptr) {
			tracked.position[0] += motion->delta_x * t;
			tracked.position[1] += motion->delta_y * t;
		}
	}
}

static void th185_player_undo() {
	for (size_t i = 0; i < th185_tracked_count; ++i) {
		float* position = th185_tracked[i].position;
		for (int axis = 0; axis < 3; ++axis)
			position[axis] = th185_tracked[i].saved[axis];
	}
}

// ---------------------------------------------------------------------------
// Straight lasers
// ---------------------------------------------------------------------------
//
// The laser manager is 0x4D7C1C. Its list starts at +0x14, the next pointer is object+0x8, and
// state 1 means finished and not drawn. Line and infinite lasers share position +0x54, angle
// +0x6C and length +0x70. Their movement is measured per frame instead of reading the different
// velocity layouts of the two classes.
static void** const th185_laser_manager = (void**)0x004D7C1C;
static const size_t th185_laser_list = 0x14;
static const size_t th185_laser_next = 0x08;
static const size_t th185_laser_state = 0x10;
static const size_t th185_laser_position = 0x54;
static const size_t th185_laser_angle = 0x6C;
static const size_t th185_laser_length = 0x70;
static const void* const th185_laser_line_vtable = (const void*)0x004B9298;
static const void* const th185_laser_infinite_vtable = (const void*)0x004B9224;
static const size_t th185_lasers_max = 0x200 + 1;

struct Th185LaserMotion {
	void* laser;
	unsigned int frame;
	float last_position[3];
	float last_angle, last_length;
	float delta_position[3];
	float delta_angle, delta_length;
};

struct Th185MovedLaser {
	void* laser;
	float position[3];
	float angle, length;
};

static Th185LaserMotion th185_laser_motions[th185_lasers_max];
static unsigned int th185_laser_motion_frame = 1;
static Th185MovedLaser th185_moved_lasers[th185_lasers_max];
static size_t th185_moved_lasers_count;
static unsigned int th185_lasers_seen;
static unsigned int th185_lasers_projected;

static bool th185_straight_laser(const void* laser) {
	const void* vtable = *(const void* const*)laser;
	return vtable == th185_laser_line_vtable || vtable == th185_laser_infinite_vtable;
}

static float th185_angle_delta(float to, float from) {
	float delta = to - from;
	while (delta > 3.1415927f)
		delta -= 6.2831855f;
	while (delta < -3.1415927f)
		delta += 6.2831855f;
	return delta;
}

static Th185LaserMotion* th185_laser_motion(void* laser) {
	Th185LaserMotion* empty = nullptr;
	Th185LaserMotion* oldest = nullptr;
	for (size_t i = 0; i < th185_lasers_max; ++i) {
		Th185LaserMotion& motion = th185_laser_motions[i];
		if (motion.laser == laser)
			return &motion;
		if (motion.laser == nullptr) {
			if (empty == nullptr)
				empty = &motion;
			continue;
		}
		if (motion.frame == th185_laser_motion_frame)
			continue;
		if (oldest == nullptr || motion.frame < oldest->frame)
			oldest = &motion;
	}
	Th185LaserMotion* motion = empty != nullptr ? empty : oldest;
	if (motion != nullptr) {
		memset(motion, 0, sizeof(*motion));
		motion->laser = laser;
	}
	return motion;
}

static void th185_lasers_measure() {
	++th185_laser_motion_frame;
	th185_lasers_seen = 0;

	void* manager = *th185_laser_manager;
	if (manager == nullptr)
		return;
	void* laser = *(void**)((char*)manager + th185_laser_list);
	for (size_t guard = 0; laser != nullptr && guard < th185_lasers_max * 4; ++guard) {
		void* next = *(void**)((char*)laser + th185_laser_next);
		if (th185_straight_laser(laser) && *(const int*)((char*)laser + th185_laser_state) != 1) {
			++th185_lasers_seen;
			float* position = (float*)((char*)laser + th185_laser_position);
			const float angle = *(float*)((char*)laser + th185_laser_angle);
			const float length = *(float*)((char*)laser + th185_laser_length);
			Th185LaserMotion* motion = th185_laser_motion(laser);
			if (motion != nullptr) {
				bool sane = motion->frame + 1 == th185_laser_motion_frame;
				for (int axis = 0; axis < 3 && sane; ++axis) {
					const float delta = position[axis] - motion->last_position[axis];
					sane = delta > -64.0f && delta < 64.0f;
				}
				if (sane) {
					for (int axis = 0; axis < 3; ++axis)
						motion->delta_position[axis] = position[axis] - motion->last_position[axis];
					motion->delta_angle = th185_angle_delta(angle, motion->last_angle);
					const float length_delta = length - motion->last_length;
					motion->delta_length = length_delta > -64.0f && length_delta < 64.0f
						? length_delta : 0.0f;
				} else {
					memset(motion->delta_position, 0, sizeof(motion->delta_position));
					motion->delta_angle = 0.0f;
					motion->delta_length = 0.0f;
				}
				motion->frame = th185_laser_motion_frame;
				for (int axis = 0; axis < 3; ++axis)
					motion->last_position[axis] = position[axis];
				motion->last_angle = angle;
				motion->last_length = length;
			}
		}
		laser = next;
	}
}

static void th185_lasers_advance(float t) {
	th185_moved_lasers_count = 0;
	th185_lasers_projected = 0;
	void* manager = *th185_laser_manager;
	if (manager == nullptr || th185_objects_held())
		return;

	void* laser = *(void**)((char*)manager + th185_laser_list);
	for (size_t guard = 0; laser != nullptr && guard < th185_lasers_max * 4; ++guard) {
		void* next = *(void**)((char*)laser + th185_laser_next);
		if (th185_straight_laser(laser) && *(const int*)((char*)laser + th185_laser_state) != 1 &&
			th185_moved_lasers_count < th185_lasers_max) {
			Th185LaserMotion* motion = th185_laser_motion(laser);
			if (motion != nullptr && motion->frame == th185_laser_motion_frame) {
				Th185MovedLaser& moved = th185_moved_lasers[th185_moved_lasers_count++];
				moved.laser = laser;
				float* position = (float*)((char*)laser + th185_laser_position);
				float* angle = (float*)((char*)laser + th185_laser_angle);
				float* length = (float*)((char*)laser + th185_laser_length);
				for (int axis = 0; axis < 3; ++axis) {
					moved.position[axis] = position[axis];
					position[axis] += motion->delta_position[axis] * t;
				}
				moved.angle = *angle;
				moved.length = *length;
				*angle += motion->delta_angle * t;
				*length += motion->delta_length * t;
				if (*length < 0.0f)
					*length = 0.0f;
				while (*angle < 0.0f)
					*angle += 6.2831855f;
				while (*angle >= 6.2831855f)
					*angle -= 6.2831855f;
				++th185_lasers_projected;
			}
		}
		laser = next;
	}
}

static void th185_lasers_undo() {
	for (size_t i = 0; i < th185_moved_lasers_count; ++i) {
		const Th185MovedLaser& moved = th185_moved_lasers[i];
		float* position = (float*)((char*)moved.laser + th185_laser_position);
		for (int axis = 0; axis < 3; ++axis)
			position[axis] = moved.position[axis];
		*(float*)((char*)moved.laser + th185_laser_angle) = moved.angle;
		*(float*)((char*)moved.laser + th185_laser_length) = moved.length;
	}
	th185_moved_lasers_count = 0;
}

// ---------------------------------------------------------------------------
// Diagnostics and extra presentations
// ---------------------------------------------------------------------------
static const unsigned int th185_diag_frames_max = 300;
static const unsigned int th185_diag_presents_max = 8;

struct Th185DiagFrame {
	LARGE_INTEGER base;
	unsigned int presents;
	LARGE_INTEGER present_time[th185_diag_presents_max];
};

static Th185DiagFrame th185_diag_frames[th185_diag_frames_max];
static unsigned int th185_diag_count;
static bool th185_diag_key_down;
static LARGE_INTEGER th185_perf_freq;

static void th185_diag_dump() {
	wchar_t path[1024] = {};
	if (!GetModuleFileNameW(NULL, path, MAX_PATH))
		return;
	wchar_t* slash = wcsrchr(path, L'\\');
	if (slash)
		slash[1] = L'\0';
	wcscat_s(path, L"oilp_diag.txt");

	FILE* file = nullptr;
	if (_wfopen_s(&file, path, L"w") != 0 || file == nullptr)
		return;

	unsigned int count = th185_diag_count < th185_diag_frames_max
		? th185_diag_count : th185_diag_frames_max;
	unsigned int start = th185_diag_count < th185_diag_frames_max
		? 0 : th185_diag_count % th185_diag_frames_max;

	fprintf(file, "oilp frame interpolation diagnostics\n");
	fprintf(file, "th185 interpolation build: generic-motion-v5\n");
	fprintf(file, "logic frame rate: %.2f, configured presentation rate: %.2f\n",
		Limiter::game_fps, Limiter::PresentRate());
	fprintf(file, "frames recorded: %u\n", count);
	fprintf(file, "bullets: %u in play, %u projected forward (last game frame)\n",
		th185_bullets_in_play, th185_bullets_projected);
	fprintf(file, "player: %u sprite(s) moved (the player, its options and shots)\n",
		(unsigned int)th185_tracked_count);
	fprintf(file, "player: %u sprite(s) had no movement record (should stay 0)\n",
		th185_sprites_untracked);
	fprintf(file, "player slots: effect sprite %u, %u option(s), %u shot(s) live\n",
		th185_main_effect_sprite, th185_options_live, th185_shots_live);
	fprintf(file, "ability manager: %u visible shot sprite(s)\n", th185_ability_sprites_live);
	fprintf(file, "ability manager detail: main %u, %u companion(s), %u active shot(s)\n",
		th185_ability_main_id, th185_ability_companions_live, th185_ability_active_shots);
	fprintf(file, "moving sprites this frame: %u sampled\n", th185_moving_sprites_count);
	for (unsigned int i = 0; i < th185_moving_sprites_count; ++i) {
		const Th185MovingSprite& moving = th185_moving_sprites[i];
		fprintf(file, "  sprite %5u at %.2f,%.2f: %.2f,%.2f\n",
			moving.id, moving.x, moving.y, moving.dx, moving.dy);
	}
	fprintf(file, "player movement: up to %.2f px per frame since the last dump\n",
		th185_player_movement);
	th185_player_movement = 0.0f;
	fprintf(file, "lasers: %u straight laser(s) in play, %u projected forward\n",
		th185_lasers_seen, th185_lasers_projected);

	double presents = 0.0;
	double first = 0.0;
	double last = 0.0;
	for (unsigned int i = 0; i < count; ++i) {
		const Th185DiagFrame& frame = th185_diag_frames[(start + i) % th185_diag_frames_max];
		fprintf(file, "frame %4u: %u present(s)", i, frame.presents);
		for (unsigned int p = 0; p < frame.presents && p < th185_diag_presents_max; ++p)
			fprintf(file, " [%u] %.2fms", p,
				(double)(frame.present_time[p].QuadPart - frame.base.QuadPart) * 1000.0 /
				(double)th185_perf_freq.QuadPart);
		fprintf(file, "\n");
		presents += frame.presents;
		if (i == 0)
			first = (double)frame.base.QuadPart;
		last = (double)frame.base.QuadPart;
	}
	if (last > first)
		fprintf(file, "average: %.3f presents per frame, %.1f presentations per second\n",
			presents / (double)count,
			presents * (double)th185_perf_freq.QuadPart / (last - first));

	fclose(file);
	printf("Wrote oilp_diag.txt\n");
}

static void th185_extra_presentation(float t) {
	IDirect3DDevice9* device = d3d9_hooked_device();
	if (device == nullptr)
		return;

	device->BeginScene();
	if (th185_scene_transforms) {
		device->SetTransform(D3DTS_VIEW, &th185_scene_view);
		device->SetTransform(D3DTS_PROJECTION, &th185_scene_projection);
	}
	if (th185_scene_viewport.Width != 0)
		device->SetViewport(&th185_scene_viewport);

	th185_reset_sprite_queue();
	*th185_render_flag = 0xFF;
	th185_draw_sprite_queue(*th185_sprite_manager);
	*th185_render_flag = 0;
	device->SetRenderState(D3DRS_FOGENABLE, 0);

	void* sprites = *th185_sprite_manager;
	if (sprites != nullptr) {
		*(float*)((char*)sprites + th185_sprite_queue) = th185_sprite_queue_saved[0];
		*(float*)((char*)sprites + th185_sprite_queue + 4) = th185_sprite_queue_saved[1];
	}

	ToolState::ReadyToDraw();
	if (Config::InterpolateBulletsAndLasers)
		th185_bullets_advance(t);
	th185_player_advance(t);
	if (Config::InterpolateBulletsAndLasers)
		th185_lasers_advance(t);
	th185_text_records_restore();
	th185_stage_save();
	th185_in_extra = true;
	th185_render_objects();
	th185_in_extra = false;
	th185_stage_restore();
	th185_lasers_undo();
	th185_player_undo();
	th185_bullets_undo();

	th185_draw_sprite_queue(*th185_sprite_manager);
	device->SetTexture(0, nullptr);
	device->EndScene();
	d3d9_present_bypassing_hooks();
	overlay_mark_presentation();
}

static HRESULT th185_present_hook() {
	IDirect3DDevice9* device = d3d9_hooked_device();
	if (device == nullptr)
		return E_FAIL;

	static bool freq_queried = false;
	if (!freq_queried) {
		freq_queried = true;
		QueryPerformanceFrequency(&th185_perf_freq);
	}

	LARGE_INTEGER base;
	QueryPerformanceCounter(&base);
	th185_player_measure();
	th185_lasers_measure();

	HRESULT result = device->Present(nullptr, nullptr, nullptr, nullptr);
	overlay_mark_presentation();
	Limiter::MarkFrameWorkEnd();
	ToolState::WatchFrameDrawn();

	unsigned int presents = 1;
	LARGE_INTEGER present_time[th185_diag_presents_max];
	present_time[0] = base;

	double rate = Limiter::PresentRate();
	if (SUCCEEDED(result) && rate > 0.0 && Limiter::game_fps > 0.0 &&
		!Limiter::PresentingEveryNthFrame()) {
		static bool rate_printed = false;
		if (!rate_printed) {
			rate_printed = true;
			printf("Frame interpolation: %.1f presentations per second (%.2f per game frame)\n",
				rate, rate / Limiter::game_fps);
		}

		LARGE_INTEGER spacing;
		unsigned int extras = Limiter::ExtraPresentations(rate, spacing);
		LARGE_INTEGER frame_wait = Limiter::FrameWait();
		if (extras > 0 && spacing.QuadPart > 0) {
			LARGE_INTEGER frame_start = Limiter::FrameStart();
			LARGE_INTEGER frame_work = Limiter::FrameWork();
			const bool has_deadline = frame_start.QuadPart > 0 && frame_work.QuadPart > 0;
			LARGE_INTEGER deadline;
			deadline.QuadPart = frame_start.QuadPart + frame_wait.QuadPart;

			for (unsigned int k = 1; k <= extras; ++k) {
				LARGE_INTEGER target;
				target.QuadPart = base.QuadPart + spacing.QuadPart * (LONGLONG)k;
				if (has_deadline && target.QuadPart + frame_work.QuadPart > deadline.QuadPart)
					continue;
				Limiter::WaitUntil(target);
				if (presents < th185_diag_presents_max)
					QueryPerformanceCounter(&present_time[presents]);
				++presents;
				th185_extra_presentation((float)k / (float)(extras + 1));
			}
		}
	}

	Th185DiagFrame& frame = th185_diag_frames[th185_diag_count % th185_diag_frames_max];
	frame.base = base;
	frame.presents = presents;
	for (unsigned int p = 0; p < th185_diag_presents_max; ++p)
		frame.present_time[p] = p < presents ? present_time[p] : base;
	++th185_diag_count;

	if (GetAsyncKeyState('U') & 0x8000) {
		if (!th185_diag_key_down) {
			th185_diag_key_down = true;
			th185_diag_dump();
		}
	} else {
		th185_diag_key_down = false;
	}
	return result;
}

static const size_t th185_present_setup_size = 14;

static void th185_skip_present_setup(DWORD address) {
	BYTE patch[th185_present_setup_size];
	patch[0] = 0xE9;
	*(DWORD*)(patch + 1) = (address + th185_present_setup_size) - (address + 5);
	for (size_t i = 5; i < th185_present_setup_size; ++i)
		patch[i] = 0x90;
	patch_bytes(address, patch, sizeof(patch));
}

void th185_install_patches() {
	{
		BYTE patch[] = { 0xEB, 0x42 };
		patch_bytes(0x004752DE, patch, sizeof(patch));
	}
	{
		BYTE patch[] = { 0xE9, 0xB2, 0x01, 0x00, 0x00, 0x90 };
		patch_bytes(0x00471A9E, patch, sizeof(patch));
	}
	patch_call(0x004741E8, th185_window_update_hook);
	if (Config::D3D9Ex) {
		patch_call(0x00473BD9, Direct3DCreate9_hook);
		BYTE patch[] = { 0x90 };
		patch_bytes(0x00473BDE, patch, sizeof(patch));
	}

	if (Config::Interpolation || Config::InterpolationFPS) {
		static const DWORD render_calls[] = { 0x004753CF, 0x0047552E };
		unsigned int render_patched = 0;
		for (DWORD address : render_calls) {
			BYTE* call = (BYTE*)address;
			if (call[0] == 0xE8) {
				patch_call(address, (void*)th185_render_hook);
				++render_patched;
			} else {
				printf("th185 render call at 0x%x doesn't look like a known one, skipping!\n",
					address);
			}
		}

		BYTE* fade_branch = (BYTE*)0x00425008;
		if (fade_branch[0] == 0x0F && fade_branch[1] == 0x8D &&
			(DWORD)0x0042500E + *(DWORD*)(fade_branch + 2) == 0x00425139) {
			patch_jmp(0x00425008, (void*)th185_fade_create_branch);
			BYTE nop = 0x90;
			patch_bytes(0x0042500D, &nop, 1);
		} else {
			printf("th185 fade branch at 0x425008 doesn't look like the known one, skipping!\n");
		}

		static const DWORD present_sites[] = { 0x0047540A, 0x004755DA };
		unsigned int present_patched = 0;
		for (DWORD address : present_sites) {
			BYTE* site = (BYTE*)address;
			if (site[0] == 0xA1 && *(DWORD*)(site + 1) == 0x004D52C8) {
				patch_call(address, (void*)th185_present_hook);
				th185_skip_present_setup(address + 5);
				++present_patched;
			} else {
				printf("th185 present call at 0x%x doesn't look like a known one, skipping!\n",
					address);
			}
		}
		printf("Frame interpolation: %u render call(s) and %u present call(s) taken over (setting %d)\n",
			render_patched, present_patched,
			Config::InterpolationFPS ? (int)Config::InterpolationFPS : Config::Interpolation);
		overlay_set_present_rate_follow_text(true);
	}
}
