// Touhou 13: Ten Desires v1.00c

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "patch_util.h"
#include "limiter.h"
#include "d3d9_hook.h"
#include "extra_state.h"
#include "config.h"
#include "overlay.h"
#include "tool_state.h"
#include "touhou13.h"

using namespace Touhou13;

CReplayManager** CReplayManager::InstancePtr = (CReplayManager**)0x004C22C8;

int __stdcall th13_window_update_hook(void* self) {
	Limiter::Tick();
	return CWindowManager__UpdateFast(self);
}

// ---------------------------------------------------------------------------
// Frame interpolation, stage 0: extra presentations
// ---------------------------------------------------------------------------
//
// th13 draws a game frame the way th14 and th15 do, with a block that sits in the frame function of
// the scene that's running:
//
//     0x45CFD6  mov eax, [0x4DC6A8]       the device
//     0x45CFDD  [vtable+0xA4]             BeginScene
//     0x45CFE6  FUN_00467960              reset the sprite queue
//     0x45CFEB  mov edi, 0x4DC6A0         the render context, for the call below
//     0x45CFF0  [0x4DCF30] = 0xFF
//     0x45CFFA  FUN_0043B080              prepare the render context (its object in EDI)
//     0x45CFFF  FUN_00470C30              render every object (they fill the sprite queue)
//     0x45D004  mov esi, [0x4DC688]       the sprite queue's owner, for the call below
//     0x45D00A  FUN_004679A0              draw the sprite queue (its object in ESI)
//     0x45D00F  [vtable+0x104]            SetTexture(0, NULL)
//     0x45D023  [vtable+0xA8]             EndScene
//
// and presents later in the same frame function (0x45D207 and the two blocks after it, one per
// menu scene), each a 5 byte load of the device followed by the arguments being pushed and the
// present called.
//
// Two things are different from th14 and th15, both the game's own doing:
//
//   - two of the helpers take their object in a register instead of on the stack, which is how the
//     compiler that built this game left them, so they can't be reached through a function pointer
//     - the trampolines below are what put the object where each one expects it.
//   - the second present site zeroes esi as part of pushing its arguments, and the code after it
//     expects that, so that site is hooked through a trampoline that does the same.
//
// The extra presentations draw the state the game is already in, which is the "moved forward by
// zero frames" case; stage 1 is what puts anything of the game's own in motion inside them.

// The plain calls the render block makes into the game
static auto th13_reset_sprite_queue = (void(*)())0x00467960;
static auto th13_render_objects = (int(*)())0x00470C30;
static void* const th13_prepare_render_address = (void*)0x0043B080;
static void* const th13_draw_sprite_queue_address = (void*)0x004679A0;

// The two helpers that want their object in a register (see the top of this file). Both registers
// are callee-saved, so the value the caller had in them is put back before returning - leaving
// them holding the object would hand the caller something else than what it had (the game's own
// call sites simply never used those registers again, which is why the game gets away with it).
static void __declspec(naked) th13_prepare_render() {
	__asm {
		push edi
		mov edi, 0x004DC6A0
		call dword ptr [th13_prepare_render_address]
		pop edi
		ret
	}
}

static void __declspec(naked) th13_draw_sprite_queue() {
	__asm {
		push esi
		mov esi, dword ptr ds:[0x004DC688]
		call dword ptr [th13_draw_sprite_queue_address]
		pop esi
		ret
	}
}

// Where the user's tool builds its frame and where it draws it. thprac's th13 module starts its
// frame from a hook on the return of the scene update at 0x470C04 and draws the overlay it built
// from a hook on the return of the object render at 0x470D27 (see tool_state.h).
static unsigned char* const th13_tool_frame_address = (unsigned char*)0x00470C04;
static unsigned char* const th13_tool_draw_address = (unsigned char*)0x00470D27;

// The viewport the scene is drawn with, taken from the frame the game's own render ran in
static D3DVIEWPORT9 th13_scene_viewport = {};

// The two transforms the game sets before its render block (its camera setup, 0x43B900, calls
// SetTransform with D3DTS_VIEW and D3DTS_PROJECTION from the matrices it keeps in the object at
// 0x4DC9D0), taken from the frame the game's own render ran in. An extra presentation runs after the
// whole frame has been drawn and a menu sets up a camera of its own, so these are put back before the
// frame is drawn again (see th13_extra_presentation).
static D3DMATRIX th13_scene_view = {};
static D3DMATRIX th13_scene_projection = {};
static bool th13_scene_transforms = false;

// The device the game draws with. The present sites load it from the game's own global, so that is
// the device that has to present the frame - not the one the patch's D3D9 hook captured, which is
// the same object while that hook is in place and nothing at all if the game got its device in a
// way the hook doesn't see. Reporting a failed present to the game ends it (its device-lost
// handling takes over), so the device the game itself would have presented with comes first.
static IDirect3DDevice9* th13_device() {
	IDirect3DDevice9* device = *(IDirect3DDevice9**)0x004DC6A8;
	if (device != nullptr)
		return device;
	return d3d9_hooked_device();
}

// The text the game's screens draw (the pause menu, the replay screens and the small line of replay
// information the save replay screen shows) is laid out through a pool of per character records in the
// text object the game keeps at [0x4C2160]. Drawing a line walks that pool, and the game rewinds it
// once per frame - that is part of what the update walk does, which an extra presentation does not
// run. An extra frame that draws the same line without rewinding the pool first takes the slice after
// the one the game's own frame took (the counter at +0x1915C goes 0 -> 0x6B -> 0xD6, so it is the
// next 107 characters each time), and every character then comes out with the animation state those
// records were left in by the frame before: the line is drawn a second time, offset more and more
// along it. This is what the game's own run of the walk starts from, saved in the render hook and put
// back before every extra presentation, so each one draws the line exactly the way that frame did.
// The fields are the ones the game's own walk was measured to change, no more.
struct Th13TextPool {
	unsigned int write;      // +0x040: where the next character's record is taken from
	float left_off_x;        // +0x050: where the text was left off
	float left_off_y;        // +0x054
	float tail[8];           // +0x3CC: what the characters drawn last left behind
	unsigned int counter;    // +0x1915C: how far into the pool this frame's text has got
};
static Th13TextPool th13_text_pool = {};

static void th13_text_pool_save() {
	void* text = *(void**)0x004C2160;
	if (text == nullptr)
		return;
	th13_text_pool.write = *(unsigned int*)((char*)text + 0x040);
	th13_text_pool.left_off_x = *(float*)((char*)text + 0x050);
	th13_text_pool.left_off_y = *(float*)((char*)text + 0x054);
	for (int i = 0; i < 8; ++i)
		th13_text_pool.tail[i] = *(float*)((char*)text + 0x3CC + i * 4);
	th13_text_pool.counter = *(unsigned int*)((char*)text + 0x1915C);
}

static void th13_text_pool_restore() {
	void* text = *(void**)0x004C2160;
	if (text == nullptr)
		return;
	*(unsigned int*)((char*)text + 0x040) = th13_text_pool.write;
	*(float*)((char*)text + 0x050) = th13_text_pool.left_off_x;
	*(float*)((char*)text + 0x054) = th13_text_pool.left_off_y;
	for (int i = 0; i < 8; ++i)
		*(float*)((char*)text + 0x3CC + i * 4) = th13_text_pool.tail[i];
	*(unsigned int*)((char*)text + 0x1915C) = th13_text_pool.counter;
}

// Remembers the viewport the scene is about to be drawn with, then lets the game's own render run
// exactly as it always did (see th13_extra_presentation for what it's for)
//
// The screen shake: the game keeps the offset it shakes the picture by in the Float2 at 0x4DCBE8 and
// zeroes it inside its own draw pass, once it has been applied (the diagnostics showed it going from
// the shake offset to 0 while the frame was drawn, and the picture an extra pass drew came out that
// many pixels away from the one the game's own pass drew - the same pattern as th08's and th20's
// shake). The value the game's pass started with is therefore put back around every extra pass.
static ExtraFloatState th13_shake = { (float*)0x004DCBE8, 2 };

// ---------------------------------------------------------------------------
// Frame interpolation: the stage object's transition state
// ---------------------------------------------------------------------------
//
// The stage object at [0x4C216C] (and the second slot at [0x4C2168], 0x41C0 bytes) has an update
// task (0x407680 -> 0x406680) and two draw tasks (0x407690 -> 0x406D90 and 0x4076A0 -> 0x407310).
// The draw ones change the object:
//
//   0x4075B4 (FUN_00407310): if ([stage+0x4054] > 0) FUN_004732C0(&[stage+0x4050], -1.0f), which
//                            subtracts one from the stage clear timer
//   0x406F99 (FUN_00406D90): if ([stage+0x404C] & 4 and [stage+0x4054] < 30) it allocates the
//                            0x44 byte fade object (0x406FA8 -> operator new) and registers its
//                            tasks with callbacks 0x45EF50/0x45F4C0
//
// The game draws once a frame, so those callbacks run once a frame. An extra presentation runs the
// render chain again, which subtracts the timer two or three times per game frame and can create
// more than one fade for the same frame. The fade object itself is fine: its own timer runs on the
// update chain, once a frame.
//
// Unlike th10-th12 the fade object is built inline instead of through a factory, so the allocation
// call at 0x406FA8 is what is guarded: returning nullptr there makes the callback take its own
// "allocation failed" path (0x407055) and skip the fade, while the rest of the draw runs normally.
static const size_t th13_stage_size = 0x41C0;
static void** const th13_stage_slots[2] = { (void**)0x004C2168, (void**)0x004C216C };
static unsigned char* th13_stage_saved[2];
static bool th13_stage_saved_valid[2];
static bool th13_in_extra;

static void th13_stage_save() {
	for (int i = 0; i < 2; ++i) {
		th13_stage_saved_valid[i] = false;
		void* stage = *th13_stage_slots[i];
		if (stage == nullptr)
			continue;
		if (th13_stage_saved[i] == nullptr)
			th13_stage_saved[i] = (unsigned char*)malloc(th13_stage_size);
		if (th13_stage_saved[i] == nullptr)
			continue;
		memcpy(th13_stage_saved[i], stage, th13_stage_size);
		th13_stage_saved_valid[i] = true;
	}
}

static void th13_stage_restore() {
	for (int i = 0; i < 2; ++i) {
		void* stage = *th13_stage_slots[i];
		if (!th13_stage_saved_valid[i] || stage == nullptr)
			continue;
		memcpy(stage, th13_stage_saved[i], th13_stage_size);
		th13_stage_saved_valid[i] = false;
	}
}

// The fade object is allocated inline by FUN_00406D90 (0x406FA8). While an extra is drawing, the
// call returns nullptr, which is exactly what the callback checks before building the fade
// (see the comment above). Everything else goes to the game's own operator new.
static void* (__cdecl* th13_operator_new)(size_t size) = (void*(__cdecl*)(size_t))0x0047A1E1;

static void* __cdecl th13_fade_alloc_guard(size_t size) {
	if (th13_in_extra)
		return nullptr;
	return th13_operator_new(size);
}

static void th13_render_hook() {
	IDirect3DDevice9* device = th13_device();
	if (device != nullptr) {
		device->GetViewport(&th13_scene_viewport);
		th13_scene_transforms = SUCCEEDED(device->GetTransform(D3DTS_VIEW, &th13_scene_view)) &&
			SUCCEEDED(device->GetTransform(D3DTS_PROJECTION, &th13_scene_projection));
	}

	ToolState::WatchFrameReady(th13_tool_frame_address, th13_tool_draw_address);

	// What the game's own run of the walk finds the text pool at, for the extra presentations to put
	// back (see Th13TextPool)
	th13_text_pool_save();
	extra_float_state_save(&th13_shake);

	th13_render_objects();
}

// Diagnostics: press U to write the presentation timing of the last frames to oilp_diag.txt
// (what the last presentation projected and moved, see the stage 1 sections below)
static unsigned int th13_bullets_in_play;
static unsigned int th13_bullets_moved;
static size_t th13_tracked_count;
static float th13_player_movement;
static unsigned int th13_sprites_untracked;
static unsigned int th13_body_sprites;
static unsigned int th13_option_sprites;
static unsigned int th13_shot_sprites;
static unsigned int th13_lasers_seen;
static unsigned int th13_lasers_projected;

static const unsigned int th13_diag_frames_max = 300;
static const unsigned int th13_diag_presents_max = 8;
struct Th13DiagFrame {
	LARGE_INTEGER base;
	unsigned int presents;
	LARGE_INTEGER present_time[th13_diag_presents_max];
};
static Th13DiagFrame th13_diag_frames[th13_diag_frames_max];
static unsigned int th13_diag_count;
static bool th13_diag_key_down;

static LARGE_INTEGER th13_perf_freq;

static void th13_diag_dump() {
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

	unsigned int count = th13_diag_count < th13_diag_frames_max ? th13_diag_count : th13_diag_frames_max;
	unsigned int start = th13_diag_count < th13_diag_frames_max ? 0 : th13_diag_count % th13_diag_frames_max;

	fprintf(file, "oilp frame interpolation diagnostics\n");
	fprintf(file, "logic frame rate: %.2f, configured presentation rate: %.2f\n",
		Limiter::game_fps, Limiter::PresentRate());
	fprintf(file, "frames recorded: %u\n", count);
	fprintf(file, "bullets: %u in play, %u projected forward (last game frame)\n",
		th13_bullets_in_play, th13_bullets_moved);
	fprintf(file, "player: %u body, %u option and %u shot sprite(s) projected (last game frame)\n",
		th13_body_sprites, th13_option_sprites, th13_shot_sprites);
	fprintf(file, "player: %u sprite(s) this frame had no movement record (should stay 0)\n",
		th13_sprites_untracked);
	fprintf(file, "player movement: up to %.2f px per frame since the last dump\n", th13_player_movement);
	fprintf(file, "lasers: %u in play, %u projected forward (last game frame)\n",
		th13_lasers_seen, th13_lasers_projected);
	th13_player_movement = 0.0f;

	double presents = 0.0;
	double first = 0.0;
	double last = 0.0;
	for (unsigned int i = 0; i < count; ++i) {
		const Th13DiagFrame& frame = th13_diag_frames[(start + i) % th13_diag_frames_max];
		fprintf(file, "frame %4u: %u present(s)", i, frame.presents);
		for (unsigned int p = 0; p < frame.presents && p < th13_diag_presents_max; ++p)
			fprintf(file, " [%u] %.2fms", p,
				(double)(frame.present_time[p].QuadPart - frame.base.QuadPart) * 1000.0 / (double)th13_perf_freq.QuadPart);
		fprintf(file, "\n");
		presents += frame.presents;
		if (i == 0)
			first = (double)frame.base.QuadPart;
		last = (double)frame.base.QuadPart;
	}
	if (last > first)
		fprintf(file, "average: %.3f presents per frame, %.1f presentations per second\n",
			presents / (double)count, presents * (double)th13_perf_freq.QuadPart / (last - first));

	fclose(file);
	printf("Wrote oilp_diag.txt\n");
}

// ---------------------------------------------------------------------------
// Frame interpolation, stage 1: project the bullets forward
// ---------------------------------------------------------------------------
//
// The bullets of a stage live in the bullet manager, which is created when a stage starts (0x40D920
// allocates it and 0x40D170 fills it in, loading bullet.anm for it) and released when it ends;
// 0x4C2174 holds it and is zero while no stage is running. The manager is the same object th14 has,
// one engine generation older: a pool of 2001 bullets of 0x135C bytes each, allocated up front and
// threaded onto a free list, plus a second list of the bullets that are in play.
//
// A bullet that is in play is entered into the second list (whose head is at manager+0x84) with a
// node that is part of the bullet itself, and the game's own update walks it that way (0x40E360):
// the node is at bullet+0x10, the node's first word is the bullet, and the node's +4 is the next
// node.
//
// A bullet keeps three floats of position (+0xB70) and three floats of velocity (+0xB7C). The
// game's own update of one bullet (0x40DB10) moves the position by the velocity once per frame,
// through the slowdown factor it applies to everything while the screen is full (0x4C0A28), and it
// moves a bullet that is waiting for its sub-pattern to start by half of that: the state at
// +0xBBE is 1 for a bullet in play, 2/3/5 for one that is starting or finishing - where the game
// itself multiplies by the double at 0x4AE8B8, which is 0.5 - and 4 or anything else doesn't move.
//
// The drawn sprites follow on their own: the bullet draw pass (0x40E4D0) copies this position into
// the animation objects the bullet carries before handing them to the sprite queue (0x46A700), so
// moving the position is enough.

static void** const th13_bullet_manager = (void**)0x004C2174;
static const size_t th13_bullet_list_head = 0x84; // in it: the first node of the bullets in play
static const size_t th13_bullet_list_next = 0x04; // in a node: the next node
static const size_t th13_bullet_position = 0xB70; // in a bullet: its position, 3 floats
static const size_t th13_bullet_velocity = 0xB7C; // in a bullet: what it moves by, 3 floats
static const size_t th13_bullet_state = 0xBBE;    // in a bullet: what it's doing, 16 bits
// The factor the game scales every movement with (1 unless the screen is full enough for it to
// slow down)
static float* const th13_time_scale = (float*)0x004C0A28;
// How many of the game's updates it runs per rendered frame, minus one
static unsigned char* const th13_frames_per_render = (unsigned char*)0x004DC890;
// The object of the pause screen while it is up, and the flags of it the bullet tasks look at
static void** const th13_pause_object = (void**)0x004C2194;
static const size_t th13_pause_flags = 0x60;

// The pool holds 2001 bullets, so the list of the ones in play can't be longer
static const size_t th13_bullets_saved_max = 2001;
struct Th13SavedBullet {
	void* bullet;
	float position[3];
};
static Th13SavedBullet th13_saved_bullets[th13_bullets_saved_max];
static size_t th13_saved_bullets_count;

// Whether the pause screen has the picture held. A picture the game isn't updating (or isn't
// drawing) must not be moved forward either. 0x40E780 and 0x40E7B0 are the game's two bullet tasks.
static bool th13_bullets_held() {
	void* pause = *th13_pause_object;
	if (pause == nullptr)
		return false;
	const unsigned int flags = *(unsigned int*)((char*)pause + th13_pause_flags);
	return ((flags | (flags >> 2)) & 1) != 0 || (flags & 4) != 0;
}

// How much of a frame's movement a bullet in this state gets (0x40DB10)
static float th13_bullet_step_factor(unsigned short state) {
	switch (state) {
	case 1:
		return 1.0f;
	case 2:
	case 3:
	case 5:
		return 0.5f;
	default:
		return 0.0f;
	}
}

// Moves every bullet in play forward by t frames and remembers what it changed
static void th13_bullets_advance(float t) {
	th13_saved_bullets_count = 0;
	th13_bullets_in_play = 0;
	th13_bullets_moved = 0;

	void* manager = *th13_bullet_manager;
	if (manager == nullptr || th13_bullets_held())
		return;

	unsigned int frames_per_render = *th13_frames_per_render;
	if (frames_per_render > 2) // the game's own setting only goes to 2
		frames_per_render = 2;

	float scale = *th13_time_scale;
	if (!(scale > 0.0f) || scale > 2.0f)
		scale = 1.0f;

	const float step = t * (float)(frames_per_render + 1) * scale;

	void* node = *(void**)((char*)manager + th13_bullet_list_head);
	for (size_t guard = 0; node != nullptr && guard < th13_bullets_saved_max * 4; ++guard) {
		void* bullet = *(void**)node;
		void* next = *(void**)((char*)node + th13_bullet_list_next);
		if (bullet == nullptr)
			break;

		++th13_bullets_in_play;
		const float factor = th13_bullet_step_factor(*(unsigned short*)((char*)bullet + th13_bullet_state));
		if (factor != 0.0f && th13_saved_bullets_count < th13_bullets_saved_max) {
			float* position = (float*)((char*)bullet + th13_bullet_position);
			const float* velocity = (const float*)((char*)bullet + th13_bullet_velocity);
			Th13SavedBullet& saved = th13_saved_bullets[th13_saved_bullets_count++];
			saved.bullet = bullet;

			const float bullet_step = step * factor;
			for (int axis = 0; axis < 3; ++axis) {
				saved.position[axis] = position[axis];
				position[axis] += velocity[axis] * bullet_step;
			}
			++th13_bullets_moved;
		}

		node = next;
	}
}

// Puts the positions of every bullet th13_bullets_advance moved back
static void th13_bullets_undo() {
	for (size_t i = 0; i < th13_saved_bullets_count; ++i) {
		const Th13SavedBullet& saved = th13_saved_bullets[i];
		float* position = (float*)((char*)saved.bullet + th13_bullet_position);
		for (int axis = 0; axis < 3; ++axis)
			position[axis] = saved.position[axis];
	}
	th13_saved_bullets_count = 0;
}

// ---------------------------------------------------------------------------
// Frame interpolation, stage 1: the player, its options and its shots
// ---------------------------------------------------------------------------
//
// The player of the stage that is running is at 0x4C22C4 (null in the menus and before a stage
// starts, 0x14844 bytes, its animation object at +0x14). The update keeps its float position at
// +0x5B8 and writes it into the sprite it wears every frame (0x442E6F), so the sprite is what
// carries the player's place on screen; the id of that sprite is at +0xA2B0 (0x442E60 looks it up
// the same way the game does). Both are moved here: the position is what the game's own movement is
// measured from, and the sprite is what the renderer reads when it draws it.
//
// Everything else the player puts on screen is positioned by the update the same way, so those
// sprites are moved themselves:
//
//   - the options (the sub-shots that sit beside the player) are 8 slots of 0xE4 bytes at +0xA2B8.
//     A slot that is up (a non-zero dword at its +0x00) keeps two sprite object ids at +0xB0/+0xB4
//     and a position in 1/128 pixel units at +0x5C, which the update walks toward the player
//     (0x442F3A). The release path at 0x444A10 and the id lookups at 0x445545 agree on +0xA2B8; the
//     +0xA380 the notes first had is +0xC8 into these same slots.
//   - the player's shots (the bullets it fires at the enemies) are 256 slots of 0x9C bytes at
//     +0x6A0. A live slot has a non-zero countdown at +0x70 (the test the update's own walk at
//     0x446790 makes), the object id of its sprite at +0x14 and its position in pixels at +0x2C,
//     which the slot's update (0x4464D0) copies into the sprite. (The +0x6C8 the notes had is that
//     same slot's +0x28 flag, and the array at 0xA9E4 is the shots' hit boxes, which are not drawn.)
//
// Neither the options nor the shots have a velocity that could be read - they follow the input and
// each other - so what each sprite moves by in a frame is measured instead: every game frame the
// positions are compared with the ones of the frame before, and the extra presentations move the
// sprites by that difference times the fraction of a frame they stand for. The shift is undone
// after every presentation, so what is measured is the game's own movement.

static void** const th13_player = (void**)0x004C22C4;
static const size_t th13_player_position = 0x5B8;       // the player's own position, 3 floats
static const size_t th13_player_body_sprite = 0xA2B0;   // the id of the sprite it wears
static const size_t th13_player_options = 0xA2B8;       // 8 slots of 0xE4 bytes
static const size_t th13_player_option_count = 8;
static const size_t th13_player_option_size = 0xE4;
static const size_t th13_player_option_live = 0x00;     // non-zero while the slot is up
static const size_t th13_player_option_sprite = 0xB0;   // 2 object ids
static const size_t th13_player_shots = 0x6A0;          // 256 slots of 0x9C bytes
static const size_t th13_player_shot_count = 256;
static const size_t th13_player_shot_size = 0x9C;
static const size_t th13_player_shot_live = 0x70;       // non-zero while the slot is in use
static const size_t th13_player_shot_sprite = 0x14;     // the sprite's object id
static const size_t th13_object_position = 0x574;       // in an animation object
static const float th13_max_step = 48.0f;

// What kind of thing a moved sprite belongs to, for the diagnostics
static const unsigned int th13_kind_body = 0;
static const unsigned int th13_kind_option = 1;
static const unsigned int th13_kind_shot = 2;

// One thing to move for an extra presentation, and what it moved by since the frame before
static const size_t th13_tracked_max =
	2 + th13_player_option_count * 2 + th13_player_shot_count;

struct Th13SpriteMotion {
	void* sprite;
	unsigned int frame;
	float last_x, last_y;
	float delta_x, delta_y;
};

struct Th13Tracked {
	float* shift;
	Th13SpriteMotion* motion;
	unsigned int kind;
};

static Th13SpriteMotion th13_motions[th13_tracked_max];
static unsigned int th13_motion_frame = 1;
static Th13Tracked th13_tracked[th13_tracked_max];
static float th13_tracked_saved[th13_tracked_max][3];

// The animation object a sprite id refers to: what the game does itself (0x46FB90), written out
// instead of called - it takes the manager in EDX and the id on the stack, which a plain call
// through a function pointer does not put together. The manager at 0x4DC688 keeps 0x1FFF objects
// of 0x5BC bytes starting at +0xDC, each with its id at +0x530 and an "in use" byte at +0x5B4; ids
// whose low 13 bits are 0x1FFF are on one of the manager's two lists instead.
static void* th13_object_by_id(unsigned int id) {
	const unsigned int index = id & 0x1FFF;
	if (id == 0 || index == 0x1FFF)
		return nullptr;

	void* manager = *(void**)0x004DC688;
	if (manager == nullptr)
		return nullptr;

	char* object = (char*)manager + 0xDC + (size_t)index * 0x5BC;
	if (*(unsigned char*)(object + 0x5B4) == 0)
		return nullptr;
	if (*(unsigned int*)(object + 0x530) != id)
		return nullptr;
	return object;
}

static Th13SpriteMotion* th13_sprite_motion(void* sprite) {
	Th13SpriteMotion* empty = nullptr;
	Th13SpriteMotion* oldest = nullptr;
	for (size_t i = 0; i < th13_tracked_max; ++i) {
		Th13SpriteMotion& motion = th13_motions[i];
		if (motion.sprite == sprite)
			return &motion;
		if (motion.sprite == nullptr) {
			if (empty == nullptr)
				empty = &motion;
			continue;
		}
		if (motion.frame == th13_motion_frame)
			continue;
		if (oldest == nullptr || motion.frame < oldest->frame)
			oldest = &motion;
	}

	Th13SpriteMotion* motion = empty != nullptr ? empty : oldest;
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

// Adds a position to what this frame moves, and works out what it moved by since the frame before.
// What is watched is the position that is moved, which is safe because every presentation puts it
// back before the frame ends.
static void th13_track(float* position, unsigned int kind) {
	if (th13_tracked_count >= th13_tracked_max)
		return;
	for (size_t i = 0; i < th13_tracked_count; ++i) {
		if (th13_tracked[i].shift == position)
			return;
	}
	Th13SpriteMotion* motion = th13_sprite_motion(position);
	if (motion == nullptr) {
		++th13_sprites_untracked;
		return;
	}

	const float x = position[0];
	const float y = position[1];
	if (motion->frame + 1 == th13_motion_frame) {
		const float dx = x - motion->last_x;
		const float dy = y - motion->last_y;
		if (dx > -th13_max_step && dx < th13_max_step && dy > -th13_max_step && dy < th13_max_step) {
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
	motion->frame = th13_motion_frame;
	motion->last_x = x;
	motion->last_y = y;

	Th13Tracked& tracked = th13_tracked[th13_tracked_count++];
	tracked.shift = position;
	tracked.motion = motion;
	tracked.kind = kind;
}

// Adds the sprite an object id names to what this frame moves, if the id names a live one
static void th13_track_object(unsigned int id, unsigned int kind) {
	void* object = th13_object_by_id(id);
	if (object == nullptr)
		return;
	th13_track((float*)((char*)object + th13_object_position), kind);
}

// Collects what is on screen this frame and works out what it moved by since the frame before.
// This runs once per game frame, from the game's own present (see th13_present_hook).
static void th13_player_measure() {
	th13_tracked_count = 0;
	th13_sprites_untracked = 0;
	++th13_motion_frame;

	void* player = *th13_player;
	if (player == nullptr)
		return;

	// The player itself: its own position is what the update moves, and the sprite it wears is
	// where that position ends up on screen
	th13_track((float*)((char*)player + th13_player_position), th13_kind_body);
	if (th13_tracked_count > 0) {
		const Th13SpriteMotion* motion = th13_tracked[0].motion;
		const float x = motion->delta_x < 0.0f ? -motion->delta_x : motion->delta_x;
		const float y = motion->delta_y < 0.0f ? -motion->delta_y : motion->delta_y;
		const float magnitude = x > y ? x : y;
		if (magnitude > th13_player_movement)
			th13_player_movement = magnitude;
	}
	th13_track_object(*(const unsigned int*)((const char*)player + th13_player_body_sprite),
		th13_kind_body);

	// the options
	for (size_t i = 0; i < th13_player_option_count; ++i) {
		const char* slot = (const char*)player + th13_player_options + i * th13_player_option_size;
		if (*(const int*)(slot + th13_player_option_live) == 0)
			continue;
		for (int sprite = 0; sprite < 2; ++sprite)
			th13_track_object(*(const unsigned int*)(slot + th13_player_option_sprite + sprite * 4),
				th13_kind_option);
	}

	// the shots
	for (size_t i = 0; i < th13_player_shot_count; ++i) {
		const char* slot = (const char*)player + th13_player_shots + i * th13_player_shot_size;
		if (*(const int*)(slot + th13_player_shot_live) == 0)
			continue;
		th13_track_object(*(const unsigned int*)(slot + th13_player_shot_sprite),
			th13_kind_shot);
	}
}

static void th13_player_advance(float t) {
	th13_body_sprites = 0;
	th13_option_sprites = 0;
	th13_shot_sprites = 0;

	for (size_t i = 0; i < th13_tracked_count; ++i) {
		const Th13Tracked& tracked = th13_tracked[i];
		float* position = tracked.shift;
		for (int axis = 0; axis < 3; ++axis)
			th13_tracked_saved[i][axis] = position[axis];
		const bool move = tracked.kind == th13_kind_shot
		                      ? Config::InterpolatePlayerShots
		                      : Config::InterpolatePlayerBody;
		if (move) {
			position[0] += tracked.motion->delta_x * t;
			position[1] += tracked.motion->delta_y * t;

			switch (tracked.kind) {
			case th13_kind_body:
				++th13_body_sprites;
				break;
			case th13_kind_option:
				++th13_option_sprites;
				break;
			default:
				++th13_shot_sprites;
				break;
			}
		}
	}
}

static void th13_player_undo() {
	for (size_t i = 0; i < th13_tracked_count; ++i) {
		float* position = th13_tracked[i].shift;
		for (int axis = 0; axis < 3; ++axis)
			position[axis] = th13_tracked_saved[i][axis];
	}
}

// ---------------------------------------------------------------------------
// Frame interpolation, stage 1: the straight lasers
// ---------------------------------------------------------------------------
//
// The lasers of a stage live in a manager of their own (0x4C22A0, 0x600 bytes, allocated by
// 0x42FC20 and filled in by 0x42FA30). It is a task: its update (0x42FE30) and its draw (0x42FEA0)
// both walk the same list, whose first laser is at manager+0x18 and whose links are the object's
// own +0x08 (next) and +0x04 (previous). Each walk calls the object's own virtual function for it -
// the update one at vtable+0x10, the draw one at vtable+0x14 - and the draw one is what matters
// here, because it is what the extra presentation runs again.
//
// A line laser's draw (0x431B00) builds its picture out of three of its own fields: the position at
// +0x50, the angle at +0x68 and the length at +0x6C. It copies the position into the sprites it
// draws and puts the far end of the beam at position + polar(angle, length), so moving those fields
// moves the laser, exactly like the bullets' position moves the bullets. The infinite laser
// (0x4337C0) reads the same position, angle and sizes.
//
// The classes, from the RTTI the game carries: LaserLineInf (vtable 0x4AB0CC), LaserInfiniteInf
// (0x4AB19C), LaserBeamInf (0x4AB204) and LaserCurveInf (0x4AB134). The curve one is the one the
// requirements say not to do, so it is left out; the beam one has no picture of its own (its draw
// is "xor eax, eax; ret") but it is a straight laser all the same.
//
// The same measurement is used as for the player, for the same reason (a laser's own update is what
// moves it, and it grows and turns as it does).

static void** const th13_laser_manager = (void**)0x004C22A0;
static const size_t th13_laser_list = 0x18;      // the first laser of the list
static const size_t th13_laser_next = 0x08;      // in a laser: the next one
static const size_t th13_laser_position = 0x50;  // where it starts, 3 floats
static const size_t th13_laser_angle = 0x68;     // the direction it points in, radians
static const size_t th13_laser_length = 0x6C;    // how far it reaches

// One class of straight laser: where it keeps what it moves by, and what it grows towards. The beam
// class is deliberately not in here: its own update (vtable+0x10, 0x437FF0) is a stub that just
// returns 0, so it never moves and there is nothing to project for it. The curve class is left out
// as the requirements ask.
struct Th13LaserLayout {
	const void* vtable;
	size_t velocity;            // 3 floats
	size_t angle_speed;         // 0 for a class that doesn't turn
	size_t length_speed;
	size_t length_limit;
	size_t length_limit_subtract;
};

static const Th13LaserLayout th13_laser_layouts[] = {
	// LaserLineInf: it doesn't turn on its own - its update (0x4315D0) never writes the angle - and
	// its length stops at what is left of 0x176 once the grown 0x78 is taken off it
	{ (const void*)0x004AB0CC, 0x5C,  0,     0x74, 0x176, 0x78 },
	// LaserInfiniteInf: turns by 0x5DC and its length stops at 0x5E0
	{ (const void*)0x004AB19C, 0x5CC, 0x5DC, 0x74, 0x5E0, 0    },
};

static const size_t th13_lasers_max = 0x200 + 1; // the manager's own limit, plus its root entry

// What one laser moved by, kept across frames (the list is walked again every frame, and a laser
// that is gone must not take its record with it - a slot that comes back later is another laser)
struct Th13LaserMoved {
	void* laser;
	float position[3];
	float angle, length;
};

static Th13LaserMoved th13_lasers_moved[th13_lasers_max];
static size_t th13_lasers_moved_count;

// The layout of the class this laser is, or null if it isn't one of the straight ones
static const Th13LaserLayout* th13_laser_layout(const void* laser) {
	const void* vtable = *(const void* const*)laser;
	for (size_t i = 0; i < sizeof(th13_laser_layouts) / sizeof(th13_laser_layouts[0]); ++i) {
		if (vtable == th13_laser_layouts[i].vtable)
			return &th13_laser_layouts[i];
	}
	return nullptr;
}

// Moves the straight lasers t frames ahead of the state the game is in, and remembers what it
// changed. Each laser is put where the velocity it carries right now would have it t frames from
// here: the game's own update adds velocity * the global scale to the position once per frame, and
// this is that same step for the fraction of a frame this presentation stands for.
static void th13_lasers_advance(float t) {
	th13_lasers_moved_count = 0;
	th13_lasers_seen = 0;
	th13_lasers_projected = 0;

	void* manager = *th13_laser_manager;
	if (manager == nullptr || th13_bullets_held())
		return;

	unsigned int frames_per_render = *th13_frames_per_render;
	if (frames_per_render > 2) // the game's own setting only goes to 2
		frames_per_render = 2;

	float scale = *th13_time_scale;
	if (!(scale > 0.0f) || scale > 2.0f)
		scale = 1.0f;

	const float step = t * (float)(frames_per_render + 1) * scale;

	void* laser = *(void**)((char*)manager + th13_laser_list);
	// The list is the game's own and this runs where nothing can change it; the bound is only so a
	// list that got broken some other way can't run away with the frame
	for (size_t guard = 0; laser != nullptr && guard < th13_lasers_max * 4; ++guard) {
		void* next = *(void**)((char*)laser + th13_laser_next);
		const Th13LaserLayout* layout = th13_laser_layout(laser);
		if (layout != nullptr && th13_lasers_moved_count < th13_lasers_max) {
			++th13_lasers_seen;
			Th13LaserMoved& moved = th13_lasers_moved[th13_lasers_moved_count++];
			moved.laser = laser;

			float* position = (float*)((char*)laser + th13_laser_position);
			float* angle = (float*)((char*)laser + th13_laser_angle);
			float* length = (float*)((char*)laser + th13_laser_length);
			for (int axis = 0; axis < 3; ++axis)
				moved.position[axis] = position[axis];
			moved.angle = *angle;
			moved.length = *length;

			const float* velocity = (const float*)((char*)laser + layout->velocity);
			for (int axis = 0; axis < 3; ++axis)
				position[axis] += velocity[axis] * step;

			if (layout->angle_speed != 0) {
				*angle += *(const float*)((char*)laser + layout->angle_speed) * step;
				// the game keeps the angle in [0, 2 pi) and hands it out from there
				while (*angle < 0.0f)
					*angle += 6.28318531f;
				while (*angle >= 6.28318531f)
					*angle -= 6.28318531f;
			}

			if (layout->length_speed != 0) {
				// Both classes stop growing at the length their own update clamps them to (the Line
				// one at what is left of 0x176 once the grown 0x78 is taken off it, the infinite one
				// at 0x5E0)
				float limit = *(const float*)((char*)laser + layout->length_limit);
				if (layout->length_limit_subtract != 0)
					limit -= *(const float*)((char*)laser + layout->length_limit_subtract);
				*length += *(const float*)((char*)laser + layout->length_speed) * step;
				if (limit > 0.0f && *length > limit)
					*length = limit;
			}
			++th13_lasers_projected;
		}
		laser = next;
	}
}

static void th13_lasers_undo() {
	for (size_t i = 0; i < th13_lasers_moved_count; ++i) {
		const Th13LaserMoved& moved = th13_lasers_moved[i];
		float* position = (float*)((char*)moved.laser + th13_laser_position);
		for (int axis = 0; axis < 3; ++axis)
			position[axis] = moved.position[axis];
		*(float*)((char*)moved.laser + th13_laser_angle) = moved.angle;
		*(float*)((char*)moved.laser + th13_laser_length) = moved.length;
	}
	th13_lasers_moved_count = 0;
}

// The camera setup (0x43B900) takes its object - the one the game keeps at 0x4DC9D0 - in EDI, which a
// plain call cannot set, so it goes through this. EDI is callee-saved, so the caller's value is put
// back before returning.
static void* const th13_camera_setup_address = (void*)0x0043B900;

static void __declspec(naked) th13_camera_setup() {
	__asm {
		push edi
		mov edi, 0x004DC9D0
		call dword ptr [th13_camera_setup_address]
		pop edi
		ret
	}
}

// One extra presentation of the frame: the same render block the game runs, once more, so the state
// the game is in is drawn and presented again, with the bullets, the player and the lasers projected
// t frames ahead of it. The tool's frame is readied again, so the object render below draws its
// overlay into this presentation the same way it does in the game's own one.
static void th13_extra_presentation(IDirect3DDevice9* device, float t) {
	device->BeginScene();
	if (th13_scene_transforms) {
		device->SetTransform(D3DTS_VIEW, &th13_scene_view);
		device->SetTransform(D3DTS_PROJECTION, &th13_scene_projection);
	}
	if (th13_scene_viewport.Width != 0)
		device->SetViewport(&th13_scene_viewport);
	// The game sets its camera up before its render block and a menu sets those up differently for
	// itself, so an extra presentation calls the game's own setup again to get the transforms above
	// plus the two values it leaves in the sprite manager that place the queue on screen.
	//
	// The queue is emptied first: the setup draws the queue once on its way in, and with nothing
	// queued that draw does nothing. The render block below empties it again anyway.
	th13_reset_sprite_queue();
	th13_camera_setup();
	th13_reset_sprite_queue();
	*(DWORD*)0x004DCF30 = 0xFF;
	th13_prepare_render();
	ToolState::ReadyToDraw();
	// Everything is drawn where it will be t frames from now; the render bakes those positions into
	// what it draws, so they can all go back right after it
	if (Config::InterpolateBulletsAndLasers) {
		th13_bullets_advance(t);
	}
	th13_player_advance(t);
	if (Config::InterpolateBulletsAndLasers) {
		th13_lasers_advance(t);
	}
	// The pool the text is laid out in goes back to what the game's own run of the walk started with,
	// so this presentation draws the same line the way that one did (see Th13TextPool)
	th13_text_pool_restore();
	// The stage's draw callbacks advance its transition (see th13_stage_save); put the object back
	// afterwards so an extra pass cannot move the transition a second or third time
	th13_stage_save();
	th13_in_extra = true;
	// Put the shake back for this pass (see the comment above)
	extra_float_state_begin(&th13_shake);
	th13_render_objects();
	extra_float_state_end(&th13_shake);
	th13_in_extra = false;
	th13_stage_restore();
	th13_player_undo();
	th13_bullets_undo();
	th13_lasers_undo();
	th13_draw_sprite_queue();
	device->SetTexture(0, nullptr);
	device->EndScene();

	// The extras go through the device's own Present, without the hooks a tool put in front of it
	// (see d3d9_present_bypassing_hooks). If the patch's D3D9 hook isn't on this device there is no
	// saved original to use, and the game's own present call is what is left.
	if (d3d9_hooked_device() == device)
		d3d9_present_bypassing_hooks();
	else
		device->Present(nullptr, nullptr, nullptr, nullptr);
	overlay_mark_presentation();
}

// Presents the frame the game just drew, and then the extra presentations the config asked for.
static HRESULT th13_present_hook() {
	IDirect3DDevice9* device = th13_device();
	if (device == nullptr)
		return E_FAIL;

	static bool freq_queried = false;
	if (!freq_queried) {
		freq_queried = true;
		QueryPerformanceFrequency(&th13_perf_freq);
	}

	LARGE_INTEGER base;
	QueryPerformanceCounter(&base);

	// The game's own update has run and it is about to put that frame on screen: this is the one
	// point per frame where the movement of the player, its options and its shots can be measured
	th13_player_measure();

	HRESULT result = device->Present(nullptr, nullptr, nullptr, nullptr);
	overlay_mark_presentation();
	Limiter::MarkFrameWorkEnd();
	ToolState::WatchFrameDrawn();

	unsigned int presents = 1;
	LARGE_INTEGER present_time[th13_diag_presents_max];
	present_time[0] = base;

	double rate = Limiter::PresentRate();
	if (SUCCEEDED(result) && rate > 0.0 && Limiter::game_fps > 0.0 && !Limiter::PresentingEveryNthFrame()) {
		static bool rate_printed = false;
		if (!rate_printed) {
			rate_printed = true;
			printf("Frame interpolation: %.1f presentations per second (%.2f per game frame)\n",
				rate, rate / Limiter::game_fps);
		}

		// How many presentations this frame gets, and how far apart they sit: the limiter works
		// both out (see Limiter::ExtraPresentations), carrying a rate that is not a multiple of
		// the game's frame rate over from frame to frame, so every game behaves the same way here
		LARGE_INTEGER spacing;
		unsigned int extras = Limiter::ExtraPresentations(rate, spacing);
		LARGE_INTEGER frame_wait = Limiter::FrameWait();
		if (extras > 0 && spacing.QuadPart > 0) {
			LARGE_INTEGER frame_start = Limiter::FrameStart();
			LARGE_INTEGER frame_work = Limiter::FrameWork();
			bool has_deadline = frame_start.QuadPart > 0 && frame_work.QuadPart > 0;
			LARGE_INTEGER deadline;
			deadline.QuadPart = frame_start.QuadPart + frame_wait.QuadPart;

			for (unsigned int k = 1; k <= extras; ++k) {
				LARGE_INTEGER target;
				target.QuadPart = base.QuadPart + spacing.QuadPart * (LONGLONG)k;
				if (has_deadline && target.QuadPart + frame_work.QuadPart > deadline.QuadPart)
					continue;

				Limiter::WaitUntil(target);

				if (presents < th13_diag_presents_max)
					QueryPerformanceCounter(&present_time[presents]);
				++presents;

				// Where this presentation sits between the game's own one and the next one:
				// the frames of one game frame are spread evenly over it, so the k-th of
				// extras + 1 of them draws the state k/(extras + 1) frames along
				th13_extra_presentation(device, (float)k / (float)(extras + 1));
			}
		}
	}

	{
		Th13DiagFrame& frame = th13_diag_frames[th13_diag_count % th13_diag_frames_max];
		frame.base = base;
		frame.presents = presents;
		for (unsigned int p = 0; p < th13_diag_presents_max; ++p)
			frame.present_time[p] = p < presents ? present_time[p] : base;
		++th13_diag_count;
	}

	if (GetAsyncKeyState('U') & 0x8000) {
		if (!th13_diag_key_down) {
			th13_diag_key_down = true;
			th13_diag_dump();
		}
	} else {
		th13_diag_key_down = false;
	}

	return result;
}

// The second present site zeroes esi among the arguments it pushes, and the code after it expects
// that, so it goes through this instead of straight to the hook
static void __declspec(naked) th13_present_hook_keeping_esi() {
	__asm {
		call th13_present_hook
		xor esi, esi
		ret
	}
}

// Jumps over the present the game sets up after its present call site, so the code after it runs
// with the result the hook left in eax. The three sites lay their arguments out over a different
// number of bytes, hence the size.
static void th13_skip_present_setup(DWORD address, size_t size) {
	BYTE patch[32];
	patch[0] = 0xE9;
	*(DWORD*)(patch + 1) = (address + size) - (address + 5);
	for (size_t i = 5; i < size; ++i)
		patch[i] = 0x90;
	patch_bytes(address, patch, size);
}

void th13_install_patches() {
	{
		// Skip the original frame limiter
		BYTE patch[] = { 0xEB, 0x5B };
		patch_bytes(0x0045D334, patch, sizeof(patch));
	}
	{
		// Force fast input latency mode
		BYTE patch[] = { 0xEB };
		patch_bytes(0x0045C5D7, patch, sizeof(patch)); // Skip over automatic
		patch_bytes(0x0045C5EB, patch, sizeof(patch)); // Skip over normal
	}
	{
		// Hook window update
		patch_call(0x0045C607, th13_window_update_hook);
	}
	if (Config::D3D9Ex) {
		// Redirect Direct3DCreate9 call
		// IAT is being hooked, but thcrap also hooks Direct3DCreate9 in the same place
		// This should force our Direct3DCreate9 hook to be loaded no matter what
		patch_call(0x0045C42F, Direct3DCreate9_hook);

		// Extra NOP is needed because we're replacing a FF 15 call, which is 6 bytes long
		BYTE patch[] = { 0x90 };
		patch_bytes(0x0045C434, patch, sizeof(patch));
	}
	if (Config::ReplaySpeedControl) {
		// Skip the original replay speed control stuff
		BYTE patch[] = { 0xEB, 0x1D };
		patch_bytes(0x00448E6F, patch, sizeof(patch));
	}
	if (Config::Interpolation || Config::InterpolationFPS) {
		// Take over the object render, only to remember the viewport the scene is drawn with (see
		// th13_render_hook); the render itself runs exactly the way it always did
		static const DWORD render_calls[] = { 0x0045CFFF, 0x0045D452, 0x0045D637 };
		unsigned int render_patched = 0;
		for (DWORD address : render_calls) {
			BYTE* render_call = (BYTE*)address;
			if (render_call[0] == 0xE8) {
				patch_call(address, (void*)th13_render_hook);
				++render_patched;
			} else {
				printf("th13 render call at 0x%x doesn't look like a known one, skipping!\n", address);
			}
		}

		// The stage's draw callback FUN_00406D90 allocates the stage clear fade at 0x406FA8; an
		// extra presentation must skip that (see th13_fade_alloc_guard)
		BYTE* fade_call = (BYTE*)0x00406FA8;
		if (fade_call[0] == 0xE8 &&
			(DWORD)0x00406FAD + *(DWORD*)(fade_call + 1) == 0x0047A1E1) {
			patch_call(0x00406FA8, (void*)th13_fade_alloc_guard);
		} else {
			printf("th13 fade allocation at 0x406FA8 doesn't look like the known one, skipping!\n");
		}

		// Take over the presents in the three frame functions. Each site starts by loading the
		// device (5 bytes, exactly what a hooked call needs) and then pushes its arguments and
		// calls Present over the number of bytes below, which are skipped over so the code after
		// them runs with the result the hook left in eax.
		static const DWORD present_sites[] = { 0x0045D207, 0x0045D4B6, 0x0045D709 };
		static const size_t present_setup_size[] = { 12, 14, 12 };
		unsigned int present_patched = 0;
		for (size_t i = 0; i < sizeof(present_sites) / sizeof(present_sites[0]); ++i) {
			BYTE* present_site = (BYTE*)present_sites[i];
			if (present_site[0] == 0xA1 && *(DWORD*)(present_site + 1) == 0x004DC6A8) {
				// The middle one also zeroes esi among its arguments (see the trampoline)
				patch_call(present_sites[i], i == 1 ? (void*)th13_present_hook_keeping_esi : (void*)th13_present_hook);
				th13_skip_present_setup(present_sites[i] + 5, present_setup_size[i]);
				++present_patched;
			} else {
				printf("th13 present call at 0x%x doesn't look like a known one, skipping!\n", present_sites[i]);
			}
		}
		printf("Frame interpolation: %u render call(s) and %u present call(s) taken over (setting %d)\n",
			render_patched, present_patched,
			Config::InterpolationFPS ? (int)Config::InterpolationFPS : Config::Interpolation);

		// The frame rate the frames are actually presented at goes right after the line the patch
		// draws in the bottom left corner (see overlay.h), the same as in th14 and th15
		// (the overlay readouts are Config::ShowOverlay's business; see Limiter::Initialize)
		overlay_set_present_rate_follow_text(true);
	}
}

FPSTarget th13_replay_callback() {
	if (*CReplayManager::InstancePtr && (*CReplayManager::InstancePtr)->mode == 1) {
		// TODO: Reverse engineer this struct instead of being lazy
		auto input = *(DWORD*)0x004E49F0;
		if (input & InputState::Focus)
			return FPSTarget::ReplaySlow;
		else if (input & (InputState::Skip | InputState::Shoot))
			return FPSTarget::ReplaySkip;
	}
	return FPSTarget::Game;
}
