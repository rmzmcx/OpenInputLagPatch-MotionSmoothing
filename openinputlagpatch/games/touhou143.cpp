// Touhou 14.3: Impossible Spell Card v1.00a

#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <d3d9.h>
#include <TlHelp32.h>
#include "patch_util.h"
#include "limiter.h"
#include "d3d9_hook.h"
#include "config.h"
#include "overlay.h"
#include "touhou143.h"

using namespace Touhou143;

CReplayManager** CReplayManager::InstancePtr = (CReplayManager**)0x004E6B98;

int __fastcall th143_window_update_hook(void* self) {
	Limiter::Tick();
	return CWindowManager__UpdateFast(self);
}

// ---------------------------------------------------------------------------
// Frame interpolation, stage 0: replay the scene render and present it again
// ---------------------------------------------------------------------------
//
// th143 has three frame functions:
//
//   0x46EC20, 0x46EFE0 and 0x46F210
//
// They share the same scene render block:
//
//   0x445190(&unk_4E4C58)       set the camera's view and projection
//   CALL [device+0xBC]          SetViewport(camera+0xE0)
//   CALL [device+0xA4]          BeginScene
//   0x47A870                   reset the sprite queue
//   [0x4E51D0] = 0xFF
//   0x444910(&dword_4E47D0)    prepare the render context
//   0x401670                   run the scene draw tasks
//   0x47A8C0([0x538DE8])       draw the queued sprites
//   CALL [device+0x104]         SetTexture(0, NULL)
//   CALL [device+0xA8]          EndScene
//
// The render task calls are at 0x46ED1D, 0x46F12D and 0x46F2CC. The scene functions
// return after EndScene, then their update helpers take the game's own Present:
//
//   0x46EF4A, 0x46F1A6 and 0x46F3BC
//
// Each Present site is a 5 byte load of the device followed by 14 bytes that pass four
// null arguments and call Present through the device vtable. The extra presentation
// repeats only the draw chain above and calls the unhooked Present, so no update task,
// input poll, timer, random number consumer, replay step or screenshot code runs.

static auto th143_reset_sprite_queue = (int(*)())0x0047A870;
static auto th143_draw_sprite_queue = (int(__fastcall*)(void*))0x0047A8C0;
static auto th143_render_objects = (int(*)())0x00401670;
static auto th143_prepare_render = (int(__thiscall*)(void*))0x00444910;
static auto th143_camera_setup = (int(__stdcall*)(void*))0x00445190;

static void** const th143_sprite_manager = (void**)0x00538DE8;
static DWORD* const th143_render_flag = (DWORD*)0x004E51D0;
static void* const th143_render_context = (void*)0x004E47D0;
static void* const th143_camera_object = (void*)0x004E4C58;
static void** const th143_camera_current = (void**)0x004E4EB0;
static DWORD* const th143_camera_kind = (DWORD*)0x004E4EB4;

// The scene transforms are taken before the game's object walk. Menus and pause
// screens can set their own camera, so an extra pass puts the scene values back.
static D3DVIEWPORT9 th143_scene_viewport = {};
static D3DMATRIX th143_scene_world = {};
static D3DMATRIX th143_scene_view = {};
static D3DMATRIX th143_scene_projection = {};
static bool th143_scene_transforms = false;

static const D3DRENDERSTATETYPE th143_saved_state_types[] = {
	D3DRS_ALPHABLENDENABLE,
	D3DRS_SRCBLEND,
	D3DRS_DESTBLEND,
	D3DRS_ZENABLE,
	D3DRS_ZWRITEENABLE,
	D3DRS_CULLMODE,
	D3DRS_LIGHTING,
	D3DRS_FOGENABLE,
	D3DRS_SCISSORTESTENABLE,
	D3DRS_COLORWRITEENABLE,
};
static const size_t th143_saved_state_count =
	sizeof(th143_saved_state_types) / sizeof(th143_saved_state_types[0]);
static DWORD th143_saved_states[th143_saved_state_count] = {};
static IDirect3DBaseTexture9* th143_scene_texture = nullptr;

static void th143_capture_scene_state() {
	IDirect3DDevice9* device = d3d9_hooked_device();
	if (device == nullptr)
		return;

	device->GetViewport(&th143_scene_viewport);
	th143_scene_transforms =
		SUCCEEDED(device->GetTransform(D3DTS_WORLD, &th143_scene_world)) &&
		SUCCEEDED(device->GetTransform(D3DTS_VIEW, &th143_scene_view)) &&
		SUCCEEDED(device->GetTransform(D3DTS_PROJECTION, &th143_scene_projection));
	for (size_t i = 0; i < th143_saved_state_count; ++i)
		device->GetRenderState(th143_saved_state_types[i], &th143_saved_states[i]);

	if (th143_scene_texture != nullptr) {
		th143_scene_texture->Release();
		th143_scene_texture = nullptr;
	}
	device->GetTexture(0, &th143_scene_texture);
}

static void th143_apply_scene_state(IDirect3DDevice9* device) {
	if (device == nullptr)
		return;
	if (th143_scene_transforms) {
		device->SetTransform(D3DTS_WORLD, &th143_scene_world);
		device->SetTransform(D3DTS_VIEW, &th143_scene_view);
		device->SetTransform(D3DTS_PROJECTION, &th143_scene_projection);
	}
	if (th143_scene_viewport.Width != 0)
		device->SetViewport(&th143_scene_viewport);
	for (size_t i = 0; i < th143_saved_state_count; ++i)
		device->SetRenderState(th143_saved_state_types[i], th143_saved_states[i]);
	device->SetTexture(0, th143_scene_texture);
}

// thprac's th143 module combines GUI construction and rendering in one hook before
// EndScene. Re-running that hook advances ImGui, while omitting it leaves the overlay
// out of two of every three presentations. th09.5 solved the same combined-hook case by
// replaying only ImGui::GetDrawData() with thprac's DX9 RenderDrawData.
//
// The supplied thprac_C9.exe is a monolithic build. Its relevant RVAs are:
//
//   +0x02A4C0  ImGui::GetDrawData()
//   +0x08CDB0  ImGui_ImplDX9_Check(device)
//   +0x08D200  ImGui_ImplDX9_RenderDrawData(draw_data)
//
// The functions are cdecl. The byte checks below make each call conditional on the
// expected code shape, so a mismatched tool build fails safely instead of jumping to a
// guessed address.
static HMODULE th143_thprac_module;
static DWORD th143_thprac_lookup_time;
static bool th143_thprac_api_checked;
static bool th143_thprac_api_ready;
static unsigned int th143_thprac_redraws;
static unsigned int th143_thprac_module_misses;
static unsigned int th143_thprac_signature_misses;
static unsigned int th143_thprac_draw_data_misses;

static bool th143_text_contains_ci(const wchar_t* text, const wchar_t* needle) {
	for (size_t start = 0; text[start] != L'\0'; ++start) {
		size_t i = 0;
		for (; needle[i] != L'\0'; ++i) {
			wchar_t a = text[start + i];
			wchar_t b = needle[i];
			if (a == L'\0')
				return false;
			if (a >= L'A' && a <= L'Z')
				a = (wchar_t)(a - L'A' + L'a');
			if (b >= L'A' && b <= L'Z')
				b = (wchar_t)(b - L'A' + L'a');
			if (a != b)
				break;
		}
		if (needle[i] == L'\0')
			return true;
	}
	return false;
}

static HMODULE th143_find_thprac_module() {
	if (th143_thprac_module != nullptr)
		return th143_thprac_module;

	const DWORD now = GetTickCount();
	if (th143_thprac_lookup_time != 0 &&
		(LONG)(now - th143_thprac_lookup_time) < 0) {
		return nullptr;
	}
	th143_thprac_lookup_time = now + 1000;

	HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, GetCurrentProcessId());
	if (snapshot == INVALID_HANDLE_VALUE) {
		++th143_thprac_module_misses;
		return nullptr;
	}

	MODULEENTRY32W entry = {};
	entry.dwSize = sizeof(entry);
	if (Module32FirstW(snapshot, &entry)) {
		do {
			if (th143_text_contains_ci(entry.szModule, L"thprac")) {
				th143_thprac_module = entry.hModule;
				break;
			}
		} while (Module32NextW(snapshot, &entry));
	}
	CloseHandle(snapshot);
	if (th143_thprac_module == nullptr)
		++th143_thprac_module_misses;
	return th143_thprac_module;
}

static bool th143_signature_matches(const unsigned char* address,
	const unsigned char* signature, const bool* mask, size_t size) {
	for (size_t i = 0; i < size; ++i) {
		if (mask[i] && address[i] != signature[i])
			return false;
	}
	return true;
}

static const unsigned char th143_thprac_get_draw_signature[] = {
	0xA1, 0x00, 0x00, 0x00, 0x00, 0x33, 0xC9, 0x8B, 0x80,
	0xF4, 0x1C, 0x00, 0x00, 0x8B, 0x00, 0x83, 0xC0, 0x34,
	0x38, 0x08, 0x0F, 0x44, 0xC1, 0xC3,
};
static const bool th143_thprac_get_draw_mask[] = {
	true, false, false, false, false, true, true, true, true,
	true, true, true, true, true, true, true, true, true,
	true, true, true, true, true, true,
};

static const unsigned char th143_thprac_check_signature[] = {
	0x83, 0xEC, 0x28, 0xA1, 0x00, 0x00, 0x00, 0x00, 0x33, 0xC4,
	0x89, 0x44, 0x24, 0x24, 0x56, 0x8B, 0x74, 0x24, 0x30, 0x39,
	0x35, 0x00, 0x00, 0x00, 0x00,
};
static const bool th143_thprac_check_mask[] = {
	true, true, true, true, false, false, false, false, true, true,
	true, true, true, true, true, true, true, true, true, true,
	true, false, false, false, false,
};

static const unsigned char th143_thprac_dx9_renderer_signature[] = {
	0x83, 0xEC, 0x30, 0xA1, 0x00, 0x00, 0x00, 0x00, 0x33, 0xC4,
	0x89, 0x44, 0x24, 0x2C, 0x55, 0x8B, 0x6C, 0x24, 0x38, 0x0F,
	0x57, 0xC0, 0x0F, 0x2F, 0x45, 0x1C,
};
static const bool th143_thprac_dx9_renderer_mask[] = {
	true, true, true, true, false, false, false, false, true, true,
	true, true, true, true, true, true, true, true, true, true,
	true, true, true, true, true, true,
};

static bool th143_prepare_thprac_api(HMODULE module) {
	if (th143_thprac_api_checked)
		return th143_thprac_api_ready;
	th143_thprac_api_checked = true;

	unsigned char* base = (unsigned char*)module;
	th143_thprac_api_ready =
		th143_signature_matches(base + 0x02A4C0,
			th143_thprac_get_draw_signature, th143_thprac_get_draw_mask,
			sizeof(th143_thprac_get_draw_signature)) &&
		th143_signature_matches(base + 0x08CDB0,
			th143_thprac_check_signature, th143_thprac_check_mask,
			sizeof(th143_thprac_check_signature)) &&
		th143_signature_matches(base + 0x08D200,
			th143_thprac_dx9_renderer_signature,
			th143_thprac_dx9_renderer_mask,
			sizeof(th143_thprac_dx9_renderer_signature));
	if (!th143_thprac_api_ready)
		++th143_thprac_signature_misses;
	return th143_thprac_api_ready;
}

static bool th143_draw_thprac_overlay() {
	HMODULE module = th143_find_thprac_module();
	if (module == nullptr)
		return false;
	if (!th143_prepare_thprac_api(module))
		return false;

	unsigned char* base = (unsigned char*)module;
	auto get_draw_data = (void*(*)())(base + 0x02A4C0);
	auto check_device = (void(__cdecl*)(IDirect3DDevice9*))(base + 0x08CDB0);
	auto render_draw_data = (void(__cdecl*)(void*))(base + 0x08D200);

	IDirect3DDevice9* device = d3d9_hooked_device();
	if (device == nullptr)
		return false;
	check_device(device);
	void* draw_data = get_draw_data();
	if (draw_data == nullptr) {
		++th143_thprac_draw_data_misses;
		return false;
	}

	render_draw_data(draw_data);
	++th143_thprac_redraws;
	return true;
}

// ---------------------------------------------------------------------------
// Frame interpolation, stage 1: project the enemy bullets forward
// ---------------------------------------------------------------------------
//
// The current stage's bullet manager is at [0x4E6A08]. Its constructor (0x412880)
// allocates 0x9BF6D0 bytes, constructs 2001 bullets of 0x13F4 bytes at manager+0x8C,
// and 0x4125C0 resets the pool and rebuilds the free list.
//
// Bullets in play are linked through a node at bullet+0x10. The first node is at
// manager+0x80, the bullet pointer is node[0], and the next node is node+4. The update
// task's walk at 0x413550 follows exactly that chain before calling 0x412A70 on each
// bullet. The draw task at 0x413730 walks six sprite lists at manager+0x14 and copies
// the bullet's logical position into the two animation objects it carries, so moving
// the logical position before the render is enough.
//
// A bullet's position is +0xBC0 (three floats), velocity +0xBCC (three floats),
// state +0xC0E (16 bits), and state timer +0x13C4. The state switch at 0x412B56 moves
// state 1 by one step, states 2 and 3 by half a step, state 5 by half a step once its
// timer reaches 3, and state 4 not at all. [0x4E40B0] is the game's global slowdown
// scale. The frame functions use the byte at 0x4E49C4 as updates per render minus one.
//
// The update and draw tasks stop for the pause object at [0x4E6A5C], flags at +0x80:
// the update task for ((flags | flags >> 2) & 1) or flags & 0x400, and the draw task
// for flags & 4. A held picture must not be projected.
static void** const th143_bullet_manager = (void**)0x004E6A08;
static const size_t th143_bullet_list_head = 0x80;
static const size_t th143_bullet_list_next = 0x04;
static const size_t th143_bullet_position = 0xBC0;
static const size_t th143_bullet_velocity = 0xBCC;
static const size_t th143_bullet_state = 0xC0E;
static const size_t th143_bullet_timer = 0x13C4;
static const size_t th143_bullet_flags = 0x20;
static float* const th143_time_scale = (float*)0x004E40B0;
static unsigned char* const th143_frames_per_render = (unsigned char*)0x004E49C4;
static void** const th143_pause_object = (void**)0x004E6A5C;
static const size_t th143_pause_flags = 0x80;

static const size_t th143_bullets_saved_max = 2000;
struct Th143SavedBullet {
	void* bullet;
	float position[3];
};
static Th143SavedBullet th143_saved_bullets[th143_bullets_saved_max];
static size_t th143_saved_bullets_count;
static unsigned int th143_bullets_in_play;
static unsigned int th143_bullets_moved;

static bool th143_bullets_held() {
	void* pause = *th143_pause_object;
	if (pause == nullptr)
		return false;
	const unsigned int flags = *(unsigned int*)((char*)pause + th143_pause_flags);
	return ((flags | (flags >> 2)) & 1) != 0 || (flags & 4) != 0 || (flags & 0x400) != 0;
}

static float th143_bullet_step_factor(
	unsigned short state, unsigned int flags, int timer) {
	switch (state) {
	case 1:
		return (flags & 0x200) != 0 ? 0.0f : 1.0f;
	case 2:
	case 3:
		return 0.5f;
	case 5:
		return timer >= 3 ? 0.5f : 0.0f;
	default:
		return 0.0f;
	}
}

static void th143_bullets_advance(float t) {
	th143_saved_bullets_count = 0;
	th143_bullets_in_play = 0;
	th143_bullets_moved = 0;

	void* manager = *th143_bullet_manager;
	if (manager == nullptr || th143_bullets_held())
		return;

	unsigned int frames_per_render = *th143_frames_per_render;
	if (frames_per_render > 2)
		frames_per_render = 2;

	float scale = *th143_time_scale;
	if (!(scale > 0.0f) || scale > 2.0f)
		scale = 1.0f;

	const float step = t * (float)(frames_per_render + 1) * scale;
	void* node = *(void**)((char*)manager + th143_bullet_list_head);
	for (size_t guard = 0; node != nullptr && guard < th143_bullets_saved_max * 4; ++guard) {
		void* bullet = *(void**)node;
		void* next = *(void**)((char*)node + th143_bullet_list_next);
		if (bullet == nullptr)
			break;

		++th143_bullets_in_play;
		const float factor = th143_bullet_step_factor(
			*(unsigned short*)((char*)bullet + th143_bullet_state),
			*(unsigned int*)((char*)bullet + th143_bullet_flags),
			*(int*)((char*)bullet + th143_bullet_timer));
		if (factor != 0.0f && th143_saved_bullets_count < th143_bullets_saved_max) {
			float* position = (float*)((char*)bullet + th143_bullet_position);
			const float* velocity = (const float*)((char*)bullet + th143_bullet_velocity);
			Th143SavedBullet& saved = th143_saved_bullets[th143_saved_bullets_count++];
			saved.bullet = bullet;

			const float bullet_step = step * factor;
			for (int axis = 0; axis < 3; ++axis) {
				saved.position[axis] = position[axis];
				position[axis] += velocity[axis] * bullet_step;
			}
			++th143_bullets_moved;
		}
		node = next;
	}
}

static void th143_bullets_undo() {
	for (size_t i = 0; i < th143_saved_bullets_count; ++i) {
		const Th143SavedBullet& saved = th143_saved_bullets[i];
		float* position = (float*)((char*)saved.bullet + th143_bullet_position);
		for (int axis = 0; axis < 3; ++axis)
			position[axis] = saved.position[axis];
	}
	th143_saved_bullets_count = 0;
}

// ---------------------------------------------------------------------------
// Frame interpolation, stage 2: the player, its options and its shots
// ---------------------------------------------------------------------------
//
// The current player is at [0x4E6B88]. Its draw task (0x44E3F0) copies the
// player's logical position at +0x5E0 into the embedded animation object before
// queueing it, so the logical position is the player's real draw source.
//
// The eight option slots start at player+0xD6EC and are 0xE4 bytes each. A live
// slot has two sprite object ids at +0xB0/+0xB4; 0x44CB40 converts their
// subpixel position at +0x5C/+0x60 and writes it into both sprite objects.
//
// The player's fire uses 256 registry slots of 0xA4 bytes at player+0xDE18.
// A live slot has bit 0 of +0x10 set, and +0x98 points back to the shot object
// that owns the visible sprite. That object keeps its sprite id at +0x14 and its
// logical position at +0x54. 0x450A70 copies the position into the sprite while
// creating or updating the shot.
//
// Sprite ids resolve through the manager at [0x538DE8], which is also the
// argument to the game's lookup at 0x484280. Its indexed objects start at +0xDC,
// are 0x5E4 bytes each, and keep the drawn position at +0x59C, the full id at
// +0x540, and the in-use byte at +0x5DC. The player's sprites use this array.
//
// The player moves by input rather than by a directly readable velocity, and
// options and shots have their own motion. Their actual draw positions are
// therefore measured once per game frame, next to the game's own Present, and
// each extra presentation applies that measured delta without running updates.
static void** const th143_player = (void**)0x004E6B88;
static const size_t th143_player_position = 0x5E0;
static const size_t th143_player_state = 0x684;
static const size_t th143_player_options = 0xD6EC;
static const size_t th143_player_option_count = 8;
static const size_t th143_player_option_size = 0xE4;
static const size_t th143_player_option_sprite = 0xB0;
static const size_t th143_player_shots = 0xDE18;
static const size_t th143_player_shot_count = 256;
static const size_t th143_player_shot_size = 0xA4;
static const size_t th143_player_shot_live = 0x10;
static const size_t th143_player_shot_owner = 0x98;
static const size_t th143_shot_sprite = 0x14;
static const size_t th143_sprite_objects = 0xDC;
static const size_t th143_sprite_size = 0x5E4;
static const size_t th143_sprite_position = 0x59C;
static const size_t th143_sprite_id = 0x540;
static const size_t th143_sprite_live = 0x5DC;
static const float th143_max_step = 48.0f;

static const size_t th143_tracked_max =
	1 + th143_player_option_count * 2 + th143_player_shot_count;

struct Th143SpriteMotion {
	void* sprite;
	unsigned int frame;
	float last_x, last_y;
	float delta_x, delta_y;
};

static Th143SpriteMotion th143_motions[th143_tracked_max];
static unsigned int th143_motion_frame = 1;
static float* th143_tracked[th143_tracked_max];
static unsigned int th143_tracked_kind[th143_tracked_max];
static size_t th143_tracked_count;
static float th143_tracked_saved[th143_tracked_max][3];

static const unsigned int th143_kind_body = 0;
static const unsigned int th143_kind_shot = 1;

static unsigned int th143_body_tracked;
static unsigned int th143_options_tracked;
static unsigned int th143_shots_in_play;
static unsigned int th143_shot_sprites_tracked;
static unsigned int th143_sprites_untracked;
static float th143_player_movement;

static void* th143_sprite_by_id(unsigned int id) {
	const unsigned int index = id & 0x1FFF;
	if (id == 0 || index == 0x1FFF)
		return nullptr;

	void* manager = *th143_sprite_manager;
	if (manager == nullptr)
		return nullptr;

	char* sprite = (char*)manager + th143_sprite_objects + (size_t)index * th143_sprite_size;
	if (*(unsigned char*)(sprite + th143_sprite_live) == 0)
		return nullptr;
	if (*(unsigned int*)(sprite + th143_sprite_id) != id)
		return nullptr;
	return sprite;
}

static Th143SpriteMotion* th143_sprite_motion(void* sprite) {
	Th143SpriteMotion* empty = nullptr;
	Th143SpriteMotion* oldest = nullptr;
	for (size_t i = 0; i < th143_tracked_max; ++i) {
		Th143SpriteMotion& motion = th143_motions[i];
		if (motion.sprite == sprite)
			return &motion;
		if (motion.sprite == nullptr) {
			if (empty == nullptr)
				empty = &motion;
			continue;
		}
		if (motion.frame == th143_motion_frame)
			continue;
		if (oldest == nullptr || motion.frame < oldest->frame)
			oldest = &motion;
	}

	Th143SpriteMotion* motion = empty != nullptr ? empty : oldest;
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

static bool th143_track(float* position, unsigned int kind) {
	if (th143_tracked_count >= th143_tracked_max)
		return false;
	for (size_t i = 0; i < th143_tracked_count; ++i) {
		if (th143_tracked[i] == position)
			return false;
	}

	Th143SpriteMotion* motion = th143_sprite_motion(position);
	if (motion == nullptr) {
		++th143_sprites_untracked;
		return false;
	}

	const float x = position[0];
	const float y = position[1];
	if (motion->frame + 1 == th143_motion_frame) {
		const float dx = x - motion->last_x;
		const float dy = y - motion->last_y;
		if (dx > -th143_max_step && dx < th143_max_step &&
			dy > -th143_max_step && dy < th143_max_step) {
			motion->delta_x = dx;
			motion->delta_y = dy;
		} else {
			motion->delta_x = 0.0f;
			motion->delta_y = 0.0f;
		}
	} else {
		motion->delta_x = 0.0f;
		motion->delta_y = 0.0f;
	}
	motion->frame = th143_motion_frame;
	motion->last_x = x;
	motion->last_y = y;

	th143_tracked[th143_tracked_count] = position;
	th143_tracked_kind[th143_tracked_count++] = kind;
	return true;
}

static void th143_player_measure() {
	th143_body_tracked = 0;
	th143_options_tracked = 0;
	th143_shots_in_play = 0;
	th143_shot_sprites_tracked = 0;
	th143_sprites_untracked = 0;
	th143_tracked_count = 0;
	++th143_motion_frame;

	void* player = *th143_player;
	if (player == nullptr)
		return;

	if (*(unsigned int*)((char*)player + th143_player_state) != 2) {
		if (th143_track((float*)((char*)player + th143_player_position), th143_kind_body)) {
			++th143_body_tracked;
			const Th143SpriteMotion* motion =
				th143_sprite_motion((char*)player + th143_player_position);
			if (motion != nullptr) {
				const float dx = motion->delta_x < 0.0f ? -motion->delta_x : motion->delta_x;
				const float dy = motion->delta_y < 0.0f ? -motion->delta_y : motion->delta_y;
				const float magnitude = dx > dy ? dx : dy;
				if (magnitude > th143_player_movement)
					th143_player_movement = magnitude;
			}
		}
	}

	for (size_t i = 0; i < th143_player_option_count; ++i) {
		const char* slot =
			(const char*)player + th143_player_options + i * th143_player_option_size;
		if (*(const unsigned int*)slot == 0)
			continue;
		for (int sprite_index = 0; sprite_index < 2; ++sprite_index) {
			const unsigned int id = *(const unsigned int*)(
				slot + th143_player_option_sprite + sprite_index * sizeof(unsigned int));
			void* sprite = th143_sprite_by_id(id);
			if (sprite == nullptr)
				continue;
			if (th143_track((float*)((char*)sprite + th143_sprite_position), th143_kind_body))
				++th143_options_tracked;
		}
	}

	for (size_t i = 0; i < th143_player_shot_count; ++i) {
		const char* slot =
			(const char*)player + th143_player_shots + i * th143_player_shot_size;
		if ((*(const unsigned int*)(slot + th143_player_shot_live) & 1) == 0)
			continue;
		++th143_shots_in_play;
		const void* owner = *(void* const*)(slot + th143_player_shot_owner);
		if (owner == nullptr)
			continue;
		const unsigned int id = *(const unsigned int*)((const char*)owner + th143_shot_sprite);
		void* sprite = th143_sprite_by_id(id);
		if (sprite == nullptr)
			continue;
		if (th143_track((float*)((char*)sprite + th143_sprite_position), th143_kind_shot))
			++th143_shot_sprites_tracked;
	}
}

static void th143_player_advance(float t) {
	for (size_t i = 0; i < th143_tracked_count; ++i) {
		float* position = th143_tracked[i];
		for (int axis = 0; axis < 3; ++axis)
			th143_tracked_saved[i][axis] = position[axis];
		const Th143SpriteMotion* motion = th143_sprite_motion(position);
		const bool move = th143_tracked_kind[i] == th143_kind_shot
		                      ? Config::InterpolatePlayerShots
		                      : Config::InterpolatePlayerBody;
		if (move && motion != nullptr) {
			position[0] += motion->delta_x * t;
			position[1] += motion->delta_y * t;
		}
	}
}

static void th143_player_undo() {
	for (size_t i = 0; i < th143_tracked_count; ++i) {
		float* position = th143_tracked[i];
		for (int axis = 0; axis < 3; ++axis)
			position[axis] = th143_tracked_saved[i][axis];
	}
}

// ---------------------------------------------------------------------------
// Frame interpolation, stage 3: the straight lasers
// ---------------------------------------------------------------------------
//
// The laser manager is at [0x4E6B6C]. Its list starts at manager+0x18, an
// object's next pointer is +0x08, and +0x10 == 1 means it has finished and is
// not drawn. Lasers queue sprite objects from their logical geometry.
//
// LaserLineInf and LaserInfiniteInf share the drawing fields the game uses:
// position +0x54 (three floats), angle +0x6C, and length/radius +0x70.
// Their vtables in this th143 executable are 0x4C5ED8 and 0x4C5E70.
// LaserBeamInf (0x4C5E08) has a stub draw and never reaches those fields.
// LaserCurveInf (0x4C5DA0) is deliberately excluded by the stage requirements.
//
// The line and infinite classes update position, angle and length in different
// places, so their actual drawing fields are measured once per game frame.
// Angle differences are normalized across the +-pi boundary. Extra
// presentations apply the measured deltas and restore all three fields.
static void** const th143_laser_manager = (void**)0x004E6B6C;
static const size_t th143_laser_list = 0x18;
static const size_t th143_laser_next = 0x08;
static const size_t th143_laser_state = 0x10;
static const size_t th143_laser_position = 0x54;
static const size_t th143_laser_angle = 0x6C;
static const size_t th143_laser_length = 0x70;
static const void* const th143_laser_line_vtable = (const void*)0x004C5ED8;
static const void* const th143_laser_infinite_vtable = (const void*)0x004C5E70;
static const size_t th143_lasers_max = 0x200 + 1;

struct Th143LaserMotion {
	void* laser;
	unsigned int frame;
	float last_position[3];
	float last_angle, last_length;
	float delta_position[3];
	float delta_angle, delta_length;
};

struct Th143MovedLaser {
	void* laser;
	float position[3];
	float angle, length;
};

static Th143LaserMotion th143_laser_motions[th143_lasers_max];
static unsigned int th143_laser_motion_frame = 1;
static Th143MovedLaser th143_moved_lasers[th143_lasers_max];
static size_t th143_moved_lasers_count;
static unsigned int th143_lasers_seen;
static unsigned int th143_lasers_projected;

static bool th143_straight_laser(const void* laser) {
	const void* vtable = *(const void* const*)laser;
	return vtable == th143_laser_line_vtable || vtable == th143_laser_infinite_vtable;
}

static bool th143_lasers_held() {
	void* pause = *th143_pause_object;
	if (pause == nullptr)
		return false;
	const unsigned int flags = *(unsigned int*)((char*)pause + th143_pause_flags);
	return (flags & (1 | 2 | 4 | 0x400)) != 0;
}

static float th143_angle_delta(float to, float from) {
	float delta = to - from;
	while (delta > 3.1415927f)
		delta -= 6.2831855f;
	while (delta < -3.1415927f)
		delta += 6.2831855f;
	return delta;
}

static Th143LaserMotion* th143_laser_motion(void* laser) {
	Th143LaserMotion* empty = nullptr;
	Th143LaserMotion* oldest = nullptr;
	for (size_t i = 0; i < th143_lasers_max; ++i) {
		Th143LaserMotion& motion = th143_laser_motions[i];
		if (motion.laser == laser)
			return &motion;
		if (motion.laser == nullptr) {
			if (empty == nullptr)
				empty = &motion;
			continue;
		}
		if (motion.frame == th143_laser_motion_frame)
			continue;
		if (oldest == nullptr || motion.frame < oldest->frame)
			oldest = &motion;
	}

	Th143LaserMotion* motion = empty != nullptr ? empty : oldest;
	if (motion == nullptr)
		return nullptr;
	memset(motion, 0, sizeof(*motion));
	motion->laser = laser;
	return motion;
}

static void th143_lasers_measure() {
	++th143_laser_motion_frame;
	th143_lasers_seen = 0;

	void* manager = *th143_laser_manager;
	if (manager == nullptr)
		return;

	void* laser = *(void**)((char*)manager + th143_laser_list);
	for (size_t guard = 0; laser != nullptr && guard < th143_lasers_max * 4; ++guard) {
		void* next = *(void**)((char*)laser + th143_laser_next);
		if (th143_straight_laser(laser) &&
			*(const int*)((char*)laser + th143_laser_state) != 1) {
			++th143_lasers_seen;
			const float* position = (const float*)((char*)laser + th143_laser_position);
			const float angle = *(const float*)((char*)laser + th143_laser_angle);
			const float length = *(const float*)((char*)laser + th143_laser_length);
			Th143LaserMotion* motion = th143_laser_motion(laser);
			if (motion != nullptr) {
				bool sane = motion->frame + 1 == th143_laser_motion_frame;
				for (int axis = 0; axis < 3 && sane; ++axis) {
					const float delta = position[axis] - motion->last_position[axis];
					sane = delta > -64.0f && delta < 64.0f;
				}
				if (sane) {
					for (int axis = 0; axis < 3; ++axis)
						motion->delta_position[axis] = position[axis] - motion->last_position[axis];
					motion->delta_angle = th143_angle_delta(angle, motion->last_angle);
					const float length_delta = length - motion->last_length;
					motion->delta_length =
						length_delta > -64.0f && length_delta < 64.0f ? length_delta : 0.0f;
				} else {
					memset(motion->delta_position, 0, sizeof(motion->delta_position));
					motion->delta_angle = 0.0f;
					motion->delta_length = 0.0f;
				}
				motion->frame = th143_laser_motion_frame;
				for (int axis = 0; axis < 3; ++axis)
					motion->last_position[axis] = position[axis];
				motion->last_angle = angle;
				motion->last_length = length;
			}
		}
		laser = next;
	}
}

static void th143_lasers_advance(float t) {
	th143_moved_lasers_count = 0;
	th143_lasers_projected = 0;
	void* manager = *th143_laser_manager;
	if (manager == nullptr || th143_lasers_held())
		return;

	void* laser = *(void**)((char*)manager + th143_laser_list);
	for (size_t guard = 0; laser != nullptr && guard < th143_lasers_max * 4; ++guard) {
		void* next = *(void**)((char*)laser + th143_laser_next);
		if (th143_straight_laser(laser) &&
			*(const int*)((char*)laser + th143_laser_state) != 1 &&
			th143_moved_lasers_count < th143_lasers_max) {
			Th143LaserMotion* motion = th143_laser_motion(laser);
			if (motion != nullptr && motion->frame == th143_laser_motion_frame) {
				Th143MovedLaser& moved = th143_moved_lasers[th143_moved_lasers_count++];
				moved.laser = laser;
				float* position = (float*)((char*)laser + th143_laser_position);
				float* angle = (float*)((char*)laser + th143_laser_angle);
				float* length = (float*)((char*)laser + th143_laser_length);
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
				++th143_lasers_projected;
			}
		}
		laser = next;
	}
}

static void th143_lasers_undo() {
	for (size_t i = 0; i < th143_moved_lasers_count; ++i) {
		const Th143MovedLaser& moved = th143_moved_lasers[i];
		float* position = (float*)((char*)moved.laser + th143_laser_position);
		for (int axis = 0; axis < 3; ++axis)
			position[axis] = moved.position[axis];
		*(float*)((char*)moved.laser + th143_laser_angle) = moved.angle;
		*(float*)((char*)moved.laser + th143_laser_length) = moved.length;
	}
	th143_moved_lasers_count = 0;
}

// The sprite manager's queue is deliberately not recreated here: 0x47A870 owns its
// initialization, and 0x47A8C0 owns both the state changes and advancement of its
// command buffers. Save the pre-present queue position and restore it after every
// extra pass, so the patch never leaves the next game frame with half-drawn batches.
struct Th143SpriteQueueState {
	bool valid;
	void* manager;
	DWORD queue_count;
	void* command_head;
	void* command_tail;
	DWORD command_count;
	void* second_head;
	void* second_tail;
	BYTE prepared;
	DWORD draw_counter;
	float origin[2];
	DWORD context_pending;
	void* camera;
	DWORD camera_kind;
	DWORD render_flag;
};

static const size_t th143_queue_count = 0xBC8200;
static const size_t th143_queue_command_head = 0xF48204;
static const size_t th143_queue_command_tail = 0xF48208;
static const size_t th143_queue_command_count = 0xF4820C;
static const size_t th143_queue_second_head = 0xFE8210;
static const size_t th143_queue_second_tail = 0xFE8214;
static const size_t th143_queue_prepared = 0xBC81A3;
static const size_t th143_queue_draw_counter = 0xCC;
static const size_t th143_queue_origin = 0xD0;
static const size_t th143_render_context_pending = 0xA00;

static Th143SpriteQueueState th143_queue_save() {
	Th143SpriteQueueState saved = {};
	saved.manager = *th143_sprite_manager;
	saved.camera = *th143_camera_current;
	saved.camera_kind = *th143_camera_kind;
	saved.render_flag = *th143_render_flag;
	saved.context_pending = *(DWORD*)((char*)th143_render_context + th143_render_context_pending);
	if (saved.manager == nullptr)
		return saved;

	const char* manager = (const char*)saved.manager;
	saved.valid = true;
	saved.queue_count = *(const DWORD*)(manager + th143_queue_count);
	saved.command_head = *(void* const*)(manager + th143_queue_command_head);
	saved.command_tail = *(void* const*)(manager + th143_queue_command_tail);
	saved.command_count = *(const DWORD*)(manager + th143_queue_command_count);
	saved.second_head = *(void* const*)(manager + th143_queue_second_head);
	saved.second_tail = *(void* const*)(manager + th143_queue_second_tail);
	saved.prepared = *(const BYTE*)(manager + th143_queue_prepared);
	saved.draw_counter = *(const DWORD*)(manager + th143_queue_draw_counter);
	saved.origin[0] = *(const float*)(manager + th143_queue_origin);
	saved.origin[1] = *(const float*)(manager + th143_queue_origin + sizeof(float));
	return saved;
}

static void th143_queue_restore(const Th143SpriteQueueState& saved) {
	*th143_camera_current = saved.camera;
	*th143_camera_kind = saved.camera_kind;
	*th143_render_flag = saved.render_flag;
	*(DWORD*)((char*)th143_render_context + th143_render_context_pending) = saved.context_pending;
	if (!saved.valid || *th143_sprite_manager != saved.manager)
		return;

	char* manager = (char*)saved.manager;
	*(DWORD*)(manager + th143_queue_count) = saved.queue_count;
	*(void**)(manager + th143_queue_command_head) = saved.command_head;
	*(void**)(manager + th143_queue_command_tail) = saved.command_tail;
	*(DWORD*)(manager + th143_queue_command_count) = saved.command_count;
	*(void**)(manager + th143_queue_second_head) = saved.second_head;
	*(void**)(manager + th143_queue_second_tail) = saved.second_tail;
	*(BYTE*)(manager + th143_queue_prepared) = saved.prepared;
	*(DWORD*)(manager + th143_queue_draw_counter) = saved.draw_counter;
	*(float*)(manager + th143_queue_origin) = saved.origin[0];
	*(float*)(manager + th143_queue_origin + sizeof(float)) = saved.origin[1];
}

// The ASCII manager appends menu text records while drawing. Its update task resets the
// record count once per game frame, so replaying only the draw task would append on top
// of the game's records. Restore the count saved before the game's own draw for each
// extra pass; the extra pass then writes the same records from the same starting count.
static void** const th143_text_object = (void**)0x004E69F8;
static const size_t th143_text_record_count = 0x191AC;
static const size_t th143_text_record_array = 0xBAC;
static unsigned int th143_text_records;

static unsigned int th143_text_count() {
	void* text = *th143_text_object;
	return text != nullptr ? *(unsigned int*)((char*)text + th143_text_record_count) : 0;
}

static void th143_text_records_save() {
	th143_text_records = th143_text_count();
}

static void th143_text_records_restore() {
	void* text = *th143_text_object;
	if (text != nullptr)
		*(unsigned int*)((char*)text + th143_text_record_count) = th143_text_records;
}

static void th143_render_hook() {
	th143_capture_scene_state();
	th143_text_records_save();
	th143_render_objects();
}

static bool th143_extra_presentation(float t) {
	IDirect3DDevice9* device = d3d9_hooked_device();
	if (device == nullptr)
		return false;

	const Th143SpriteQueueState queue = th143_queue_save();

	// This is the same order as the scene block: flush the old queue, set up the scene
	// camera, then begin and fill a fresh queue.
	void* manager = *th143_sprite_manager;
	if (manager != nullptr)
		th143_draw_sprite_queue(manager);
	*th143_camera_current = th143_camera_object;
	th143_camera_setup(th143_camera_object);
	*th143_camera_kind = 2;

	device->BeginScene();
	th143_apply_scene_state(device);
	th143_reset_sprite_queue();
	*th143_render_flag = 0xFF;
	th143_prepare_render(th143_render_context);

	th143_text_records_restore();
	if (Config::InterpolateBulletsAndLasers)
		th143_bullets_advance(t);
	th143_player_advance(t);
	if (Config::InterpolateBulletsAndLasers)
		th143_lasers_advance(t);
	th143_render_objects();
	th143_lasers_undo();
	th143_player_undo();
	th143_bullets_undo();
	if (manager != nullptr)
		th143_draw_sprite_queue(manager);
	device->SetTexture(0, nullptr);
	th143_draw_thprac_overlay();
	device->EndScene();

	th143_queue_restore(queue);
	th143_apply_scene_state(device);
	const HRESULT result = d3d9_present_bypassing_hooks();
	if (SUCCEEDED(result))
		overlay_mark_presentation();
	return SUCCEEDED(result);
}

static const unsigned int th143_diag_frames_max = 300;
static const unsigned int th143_diag_presents_max = 8;
struct Th143DiagFrame {
	LARGE_INTEGER base;
	unsigned int presents;
	LARGE_INTEGER present_time[th143_diag_presents_max];
};
static Th143DiagFrame th143_diag_frames[th143_diag_frames_max];
static unsigned int th143_diag_count;
static bool th143_diag_key_down;
static LARGE_INTEGER th143_perf_freq;

static void th143_diag_dump() {
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

	unsigned int count = th143_diag_count < th143_diag_frames_max
		? th143_diag_count : th143_diag_frames_max;
	unsigned int start = th143_diag_count < th143_diag_frames_max
		? 0 : th143_diag_count % th143_diag_frames_max;

	fprintf(file, "oilp frame interpolation diagnostics\n");
	fprintf(file, "th143 interpolation build: stage3-lasers-v1\n");
	fprintf(file, "logic frame rate: %.2f, configured presentation rate: %.2f\n",
		Limiter::game_fps, Limiter::PresentRate());
	fprintf(file, "frames recorded: %u\n", count);
	fprintf(file, "text records saved before scene draw: %u\n", th143_text_records);
	fprintf(file, "bullets: %u in play, %u projected forward (last game frame)\n",
		th143_bullets_in_play, th143_bullets_moved);
	fprintf(file, "player: body=%u, option sprites=%u moved, %u sprite(s) untracked\n",
		th143_body_tracked, th143_options_tracked, th143_sprites_untracked);
	fprintf(file, "player shots: %u registry slot(s), %u sprite(s) moved\n",
		th143_shots_in_play, th143_shot_sprites_tracked);
	fprintf(file, "player movement: up to %.2f px per frame since the last dump\n",
		th143_player_movement);
	th143_player_movement = 0.0f;
	fprintf(file, "lasers: %u Line/Infinite in play, %u projected forward "
		"(Curve and Beam are left alone)\n",
		th143_lasers_seen, th143_lasers_projected);
	fprintf(file, "thprac_C9 draw-only: %u redraw(s), module-misses=%u, "
		"signature-misses=%u, draw-data-misses=%u\n",
		th143_thprac_redraws, th143_thprac_module_misses,
		th143_thprac_signature_misses, th143_thprac_draw_data_misses);

	double presents = 0.0;
	double first = 0.0;
	double last = 0.0;
	for (unsigned int i = 0; i < count; ++i) {
		const Th143DiagFrame& frame =
			th143_diag_frames[(start + i) % th143_diag_frames_max];
		fprintf(file, "frame %4u: %u present(s)", i, frame.presents);
		for (unsigned int p = 0; p < frame.presents && p < th143_diag_presents_max; ++p)
			fprintf(file, " [%u] %.2fms", p,
				(double)(frame.present_time[p].QuadPart - frame.base.QuadPart) * 1000.0 /
				(double)th143_perf_freq.QuadPart);
		fprintf(file, "\n");
		presents += frame.presents;
		if (i == 0)
			first = (double)frame.base.QuadPart;
		last = (double)frame.base.QuadPart;
	}
	if (last > first)
		fprintf(file, "average: %.3f presents per frame, %.1f presentations per second\n",
			presents / (double)count,
			presents * (double)th143_perf_freq.QuadPart / (last - first));

	fclose(file);
	printf("Wrote oilp_diag.txt\n");
}

static HRESULT th143_present_hook() {
	IDirect3DDevice9* device = d3d9_hooked_device();
	if (device == nullptr)
		return E_FAIL;

	static bool freq_queried = false;
	if (!freq_queried) {
		freq_queried = true;
		QueryPerformanceFrequency(&th143_perf_freq);
	}

	LARGE_INTEGER base;
	QueryPerformanceCounter(&base);

	th143_player_measure();
	th143_lasers_measure();
	HRESULT result = device->Present(nullptr, nullptr, nullptr, nullptr);
	overlay_mark_presentation();
	Limiter::MarkFrameWorkEnd();

	unsigned int presents = 1;
	LARGE_INTEGER present_time[th143_diag_presents_max];
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
				if (th143_extra_presentation((float)k / (float)(extras + 1))) {
					if (presents < th143_diag_presents_max)
						QueryPerformanceCounter(&present_time[presents]);
					++presents;
				}
			}
		}
	}

	Th143DiagFrame& frame = th143_diag_frames[th143_diag_count % th143_diag_frames_max];
	frame.base = base;
	frame.presents = presents;
	for (unsigned int p = 0; p < th143_diag_presents_max; ++p)
		frame.present_time[p] = p < presents ? present_time[p] : base;
	++th143_diag_count;

	if (GetAsyncKeyState('U') & 0x8000) {
		if (!th143_diag_key_down) {
			th143_diag_key_down = true;
			th143_diag_dump();
		}
	} else {
		th143_diag_key_down = false;
	}

	return result;
}

static void th143_skip_present_setup(DWORD address) {
	const size_t size = 14;
	BYTE patch[size];
	patch[0] = 0xE9;
	*(DWORD*)(patch + 1) = (address + size) - (address + 5);
	for (size_t i = 5; i < size; ++i)
		patch[i] = 0x90;
	patch_bytes(address, patch, sizeof(patch));
}

void th143_install_patches() {
	{
		// Skip the original frame limiter
		BYTE patch[] = { 0xEB, 0x4A };
		patch_bytes(0x0046F02E, patch, sizeof(patch));
	}
	{
		// Force fast input latency mode
		BYTE patch[] = { 0xEB };
		patch_bytes(0x0046E2E6, patch, sizeof(patch)); // Skip over automatic
		patch_bytes(0x0046E304, patch, sizeof(patch)); // Skip over normal
	}
	{
		// Hook window update
		patch_call(0x0046E317, th143_window_update_hook);
	}
	if (Config::D3D9Ex) {
		// Redirect Direct3DCreate9 call
		// IAT is being hooked, but thcrap also hooks Direct3DCreate9 in the same place
		// This should force our Direct3DCreate9 hook to be loaded no matter what
		patch_call(0x0046DDEC, Direct3DCreate9_hook);

		// Extra NOP is needed because we're replacing a FF 15 call, which is 6 bytes long
		BYTE patch[] = { 0x90 };
		patch_bytes(0x0046DDF1, patch, sizeof(patch));
	}
	if (Config::ReplaySpeedControl) {
		// Skip the original replay speed control stuff
		BYTE patch[] = { 0xEB, 0x1A };
		patch_bytes(0x00455462, patch, sizeof(patch));
	}
	if (Config::Interpolation || Config::InterpolationFPS) {
		static const DWORD render_calls[] = {
			0x0046ED1D, 0x0046F12D, 0x0046F2CC
		};
		unsigned int render_patched = 0;
		for (DWORD address : render_calls) {
			BYTE* call = (BYTE*)address;
			if (call[0] == 0xE8 &&
				address + 5 + *(DWORD*)(call + 1) == (DWORD)th143_render_objects) {
				patch_call(address, (void*)th143_render_hook);
				++render_patched;
			} else {
				printf("th143 render call at 0x%x doesn't look like the known one, skipping!\n",
					address);
			}
		}

		static const DWORD present_sites[] = {
			0x0046EF3A, 0x0046F196, 0x0046F3AC
		};
		unsigned int present_patched = 0;
		for (DWORD address : present_sites) {
			BYTE* site = (BYTE*)address;
			if (site[0] == 0xA1 && *(DWORD*)(site + 1) == 0x004E47D8) {
				patch_call(address, (void*)th143_present_hook);
				th143_skip_present_setup(address + 5);
				++present_patched;
			} else {
				printf("th143 present site at 0x%x doesn't look like the known one, skipping!\n",
					address);
			}
		}
		printf("Frame interpolation: %u render call(s) and %u present call(s) taken over (setting %d)\n",
			render_patched, present_patched,
			Config::InterpolationFPS ? (int)Config::InterpolationFPS : Config::Interpolation);
		overlay_set_present_rate_follow_text(true);
	}
}

FPSTarget th143_replay_callback() {
	if (*CReplayManager::InstancePtr && (*CReplayManager::InstancePtr)->mode == 1) {
		// TODO: Reverse engineer this struct instead of being lazy
		auto input = *(DWORD*)0x004E19D0;
		if (input & InputState::Focus)
			return FPSTarget::ReplaySlow;
		else if (input & (InputState::Skip | InputState::Shoot))
			return FPSTarget::ReplaySkip;
	}
	return FPSTarget::Game;
}
