// Touhou 16: Hidden Star in Four Seasons v1.00a

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
#include "touhou16.h"

using namespace Touhou16;

CReplayManager** CReplayManager::InstancePtr = (CReplayManager**)0x004A6F08;

int __fastcall th16_window_update_hook(void* self) {
	Limiter::Tick();
	return CWindowManager__UpdateFast(self);
}

// ---------------------------------------------------------------------------
// Frame interpolation, stage 0: extra presentations
// ---------------------------------------------------------------------------
//
// th16 draws a game frame once per frame, with a block that sits in the frame function of the scene
// that's running:
//
//     0x45A97D  CALL [device+0xA4]        BeginScene
//     0x45A983  FUN_00465A30              reset the sprite queue
//     0x45A997  FUN_0043C4F0(0x4C10D0)    prepare the render context
//     0x45A99C  FUN_004015A0              render every object (they fill the sprite queue)
//     0x45A9A7  FUN_00465A80([0x4C0F48])  draw the sprite queue
//     0x45A9B8  CALL [device+0x104]       SetTexture(0, NULL)
//     0x45A9C6  CALL [device+0xA8]        EndScene
//
// with the device loaded from 0x4C10D8 and the byte the block sets to 0xFF at 0x4C1ADC. Just before
// the block the frame function sets the camera up (0x43C710, called with the camera object at
// 0x4C1560): that pushes the view and projection transforms into the device and hands the sprite
// manager the two values that place its queue on screen, and the frame function follows it with
// SetViewport of the viewport at 0x4C1640.
//
// The block appears three times, once for the game scene and once for each of the two menu scenes:
// the renders are at 0x45A99C, 0x45AD9C and 0x45AF28, and the presentations - the game's own
// Present call - at 0x45ABAA, 0x45ADF6 and 0x45AFFB. Each present site is laid out identically: the
// device is loaded into eax, the arguments are pushed, then the present is called through the
// device's vtable, 14 bytes in all after the load.
//
// zun's engine separates the game's logic from its rendering, so the same state can be drawn again
// by running the render block again - that's what the extra presentations are. The game's own
// present is the first one, and the extra ones are spread evenly over the rest of the frame, right
// where the game would otherwise sit and wait for the next one, so the presents land on the
// display's refresh evenly:
//
//     |-----------------------|-----------------------|-----------------------|
//     present                 present                 present                 present
//     (the game's own)        (extra)                 (extra)                 (the next frame's)
//
// The projection of the drawn objects - moving them forward in time before the extra presentations
// - comes with stage 1. With stage 0 the extra presentations draw the state the game is already in,
// which is the "moved forward by zero frames" case, and is what the infrastructure - the pacing,
// the re-rendering, the overlays of the user's tools, the diagnostics - is validated with.

// The plain calls the render block makes into the game
static auto th16_reset_sprite_queue = (void(*)())0x00465A30;
static auto th16_draw_sprite_queue = (void(__fastcall*)(void*))0x00465A80; // ECX = the sprite manager
static auto th16_prepare_render = (void(__fastcall*)(void*))0x0043C4F0;    // ECX = the render context
static auto th16_render_objects = (void(*)())0x004015A0;                   // renders every object
// The camera setup, called with the camera object; it is __stdcall (it cleans its argument up)
static auto th16_camera_setup = (void(__stdcall*)(void*))0x0043C710;

static void* const th16_camera_object = (void*)0x004C1560;
static void* const th16_render_context = (void*)0x004C10D0;
static void** const th16_sprite_manager = (void**)0x004C0F48;
// The dword the render block sets to 0xFF
static DWORD* const th16_render_flag = (DWORD*)0x004C1ADC;

// Where the user's tool builds its frame and where it draws it. thprac's th16 module starts its
// frame (its NewFrame, its UI code, and its EndFrame) from a hook on the return of the scene
// update at 0x40156F, and draws the overlay it built from a hook on the return of the object
// render at 0x40168A. The extra presentations render the scene again, which erases the overlay the
// game's own pass drew, so the tool has to draw it again for every one of them (see tool_state.h -
// its frame state is found at runtime and set again there, which draws the overlay without running
// any of the tool's own code).
static unsigned char* const th16_tool_frame_address = (unsigned char*)0x0040156F;
static unsigned char* const th16_tool_draw_address = (unsigned char*)0x0040168A;

// The viewport the scene is drawn with, taken from the frame the game's own render ran in: the
// extra presentations have to draw with the same one, and it can differ from what's left over in
// the device (the pause screens, for example, render into a smaller part of the back buffer)
static D3DVIEWPORT9 th16_scene_viewport = {};

// The two transforms the game sets before its render block (the camera setup calls SetTransform
// with D3DTS_VIEW and D3DTS_PROJECTION), taken from the frame the game's own render ran in. An
// extra presentation runs after the whole frame has been drawn and a menu sets up a camera of its
// own, so these are put back before the frame is drawn again (see th16_extra_presentation).
static D3DMATRIX th16_scene_view = {};
static D3DMATRIX th16_scene_projection = {};
static bool th16_scene_transforms = false;

// The menus draw their text through a list of records the game keeps in the text object it holds
// at [0x4A6D98]: drawing a string appends a record (0x138 bytes each, at +0xBF0 - the append is
// FUN_00408140) and bumps the frame's count of them (+0x19208), and the object's own draw then
// walks the records that count covers. The appends happen while the frame is being drawn, so an
// extra presentation that draws the same screen appends its own records after the ones the game's
// own pass left behind and draws both - the same text twice, which the alpha blending these
// screens use turns into text that looks a little denser than the game's own frame shows, or,
// where the second batch is placed differently, into a second copy of it.
//
// So the count the game's own pass started from is kept here and put back before every extra
// presentation: each one then appends its own records from the start of the list, exactly the way
// the game's own frame did, and what it leaves behind is what the game's own pass left.
static const size_t th16_text_record_count = 0x19208;
static unsigned int th16_text_records;

static unsigned int th16_text_count() {
	void* text = *(void**)0x004A6D98;
	return text != nullptr ? *(unsigned int*)((char*)text + th16_text_record_count) : 0;
}

static void th16_text_records_save() {
	th16_text_records = th16_text_count();
}

static void th16_text_records_restore() {
	void* text = *(void**)0x004A6D98;
	if (text != nullptr)
		*(unsigned int*)((char*)text + th16_text_record_count) = th16_text_records;
}

// Remembers the viewport the scene is about to be drawn with, then lets the game's own render run
// exactly as it always did (see th16_extra_presentation for what it's for)
// The screen shake: the game keeps the offset it shakes the picture by in the Float2 at 0x4C1788 and
// zeroes it inside its own draw pass, once it has been applied (the diagnostics showed it going from
// the shake offset to 0 while the frame was drawn, and the picture an extra pass drew came out that
// many pixels away from the one the game's own pass drew - the same pattern as th08's and th20's
// shake). The value the game's pass started with is therefore put back around every extra pass.
static ExtraFloatState th16_shake = { (float*)0x004C1788, 2 };

// ---------------------------------------------------------------------------
// Frame interpolation: the stage object's transition state
// ---------------------------------------------------------------------------
//
// The stage object at [0x4A6DA0] (and the second slot at [0x4A6D9C], 0x66F0 bytes) is drawn by
// the callback at 0x409F90, which changes the object in two places:
//
//   0x40A6D0: if ([stage+0x66CC] > 0) it subtracts one from the stage clear timer at
//             [stage+0x66C8]
//   0x40A1AB: if ([stage+0x66C4] & 4 and [stage+0x66CC] < 30) it allocates the 0x40 byte fade
//             object at 0x40A1BA and registers its tasks
//
// The game draws once a frame, so those callbacks run once a frame. An extra presentation runs the
// render chain again, which subtracts the timer two or three times per game frame and can create
// more than one fade for the same frame. The fade object itself is fine: its own timer runs on the
// update chain, once a frame.
//
// Only the transition fields are put back, not the whole object: restoring the object's pointers
// put back handles to objects the draw pass may have replaced, which crashed when the next stage
// loaded. The two ranges are the flag/timer block at +0x66B8 and the fade colour/alpha byte at
// +0x3348.
static const size_t th16_stage_ranges[][2] = { { 0x66B8, 0x38 }, { 0x3348, 0x10 } };
static const size_t th16_stage_range_count = sizeof(th16_stage_ranges) / sizeof(th16_stage_ranges[0]);
static const size_t th16_stage_saved_size = 0x48;
static void** const th16_stage_slots[2] = { (void**)0x004A6D9C, (void**)0x004A6DA0 };
static unsigned char* th16_stage_saved[2];
static bool th16_stage_saved_valid[2];
static bool th16_in_extra;

static void th16_stage_save() {
	for (int i = 0; i < 2; ++i) {
		th16_stage_saved_valid[i] = false;
		void* stage = *th16_stage_slots[i];
		if (stage == nullptr)
			continue;
		if (th16_stage_saved[i] == nullptr)
			th16_stage_saved[i] = (unsigned char*)malloc(th16_stage_saved_size);
		if (th16_stage_saved[i] == nullptr)
			continue;
		size_t offset = 0;
		for (size_t r = 0; r < th16_stage_range_count; ++r) {
			memcpy(th16_stage_saved[i] + offset, (char*)stage + th16_stage_ranges[r][0],
				th16_stage_ranges[r][1]);
			offset += th16_stage_ranges[r][1];
		}
		th16_stage_saved_valid[i] = true;
	}
}

static void th16_stage_restore() {
	for (int i = 0; i < 2; ++i) {
		void* stage = *th16_stage_slots[i];
		if (!th16_stage_saved_valid[i] || stage == nullptr)
			continue;
		size_t offset = 0;
		for (size_t r = 0; r < th16_stage_range_count; ++r) {
			memcpy((char*)stage + th16_stage_ranges[r][0], th16_stage_saved[i] + offset,
				th16_stage_ranges[r][1]);
			offset += th16_stage_ranges[r][1];
		}
		th16_stage_saved_valid[i] = false;
	}
}

// The fade creation block does not check its allocation for null (it dereferences the result
// immediately), so it cannot be skipped by making the allocation fail. Instead the branch that
// guards the block (0x40A1B2, `jge 0x40A2AD`) is replaced with a jump to this trampoline: during an
// extra pass it always takes the "no fade" path; otherwise it runs the game's own condition.
static __declspec(noinline) bool th16_is_extra() {
	return th16_in_extra;
}

static void __declspec(naked) th16_fade_create_branch() {
	__asm {
		push eax
		push ecx
		push edx
		call th16_is_extra
		test al, al
		pop edx
		pop ecx
		pop eax
		jne skip
		cmp dword ptr [edi + 66CCh], 1Eh
		jge skip
		mov ecx, 0040A1B8h
		jmp ecx
	skip:
		mov ecx, 0040A2ADh
		jmp ecx
	}
}

static void th16_render_hook() {
	IDirect3DDevice9* device = d3d9_hooked_device();
	if (device != nullptr) {
		device->GetViewport(&th16_scene_viewport);
		th16_scene_transforms = SUCCEEDED(device->GetTransform(D3DTS_VIEW, &th16_scene_view)) &&
			SUCCEEDED(device->GetTransform(D3DTS_PROJECTION, &th16_scene_projection));
	}

	// The tool's frame is built by now (it runs from the scene update's return, before this) but
	// not drawn yet (that happens on the return of the render below): the point to watch it from
	ToolState::WatchFrameReady(th16_tool_frame_address, th16_tool_draw_address);

	// What the game's own run of the walk finds the text list at, for the extra presentations to
	// put back (see th16_text_record_count)
	th16_text_records_save();
	extra_float_state_save(&th16_shake);

	th16_render_objects();
}

// ---------------------------------------------------------------------------
// Frame interpolation, stage 1: project the bullets forward
// ---------------------------------------------------------------------------
//
// The bullets of a stage live in the bullet manager, which is created when a stage starts
// (0x411DD0 allocates it and 0x411CA0 releases it) and whose object is kept at 0x4A6DAC; the
// manager is built by 0x411880, which fills the pools and puts itself in that global.
//
// The manager owns two pools of 2001 bullets each (0x1478 bytes each, the first at manager+0x9C
// and the second at manager+0x9FFE94, the second one being the copy the game keeps for its replay
// snapshots), and the bullets that are in play are threaded onto a list of their own: its first
// node is at manager+0x70, the node is part of the bullet itself (bullet+0x10, with the bullet at
// node[0] and the next node at node[4]), which is how the game's own update walks it (0x412860:
// `mov esi, [edi+70h]` / `mov esi, [esi]` / `mov eax, [esi+4]`). The manager's own constructor
// (0x411A30) threads the free list through the same bullets (that one at manager+0x60, with the
// bullet's +0x04/+0x08 as its links).
//
// A bullet keeps three floats of position and three floats of velocity. The game moves the
// position by the velocity once per frame, scaled by the slowdown factor it applies to everything
// while the screen is full (0x411E70, the update of one bullet: case 1 of the state switch adds
// velocity * [0x4A5788] to position, and it is the state switch on the 16 bit state at +0xC72 that
// decides how much a bullet moves). This is the movement that has to be reproduced for the extra
// presentations: they draw the state the game is in at the moment of the game's own present, so the
// state a fraction of a frame later is the same state with every bullet moved by velocity * that
// fraction. The game's own logic is never given the moved positions - they are put back right after
// the extra presentation's render.
//
// Two things about the game matter here and are reproduced:
//
//   - The game only renders every frames_per_render + 1 of its updates (the byte at 0x4C12C4,
//     which the three frame functions compare their counter against), so the step between two
//     rendered frames is that many of its update steps. (The byte is never written in this game,
//     so it stays 0, but the code keeps the general form the other games have.)
//   - While the pause screen holds the picture the bullets are neither updated nor drawn: the two
//     bullet tasks of the game (0x412C50 and 0x412C80) test the pause object at 0x4A6DD4 for
//     that, and its walk skips every bullet on 0x400. A bullet that is waiting for its sub-pattern
//     to start is moved by half a step (the switch's states 2, 3 and 5, which scale by the 0.5
//     at 0x49449C), and one whose 0x200 flag is set is held - the game neither moves nor draws it.
//
// The drawn sprites follow the position on their own: the bullet draw pass (0x412A60) copies
// position into the animation objects the bullet carries before handing them to the sprite queue.

// The bullet manager of the stage that is running (null while no stage is)
static void** const th16_bullet_manager = (void**)0x004A6DAC;
static const size_t th16_bullet_list_head = 0x70; // in it: the first node of the bullets in play
static const size_t th16_bullet_list_next = 0x04; // in a node: the next node
static const size_t th16_bullet_position = 0xC20; // in a bullet: its position, 3 floats
static const size_t th16_bullet_velocity = 0xC2C; // in a bullet: what it moves by, 3 floats
static const size_t th16_bullet_state = 0xC72;    // in a bullet: what it's doing, 16 bits
static const size_t th16_bullet_timer = 0x1450;   // in a bullet: how long it has been doing it
static const size_t th16_bullet_flags = 0x20;     // in a bullet: 0x200 holds it (and hides it)
// The factor the game scales every movement with (1 unless the screen is full enough for it to
// slow down)
static float* const th16_time_scale = (float*)0x004A5788;
// How many of the game's updates it runs per rendered frame, minus one
static unsigned char* const th16_frames_per_render = (unsigned char*)0x004C12C4;
// The object of the pause screen while it is up, and the flags of it the bullet tasks look at
static void** const th16_pause_object = (void**)0x004A6DD4;
static const size_t th16_pause_flags = 0x88;

// The bullets that were moved for the presentation being drawn, and the positions they had. The
// pool holds 2001 of them, so the list of the ones in play can't be longer; only the bullets that
// were saved are moved, so a list that somehow grew past the array would only cost the projection
// of the bullets at its end.
static const size_t th16_bullets_saved_max = 2001;
struct Th16SavedBullet {
	void* bullet;
	float position[3];
};
static Th16SavedBullet th16_saved_bullets[th16_bullets_saved_max];
static size_t th16_saved_bullets_count;

// What the last presentation projected, for the diagnostics (see th16_diag_dump)
static unsigned int th16_bullets_in_play;
static unsigned int th16_bullets_moved;

// Whether the pause screen has the picture held. A picture the game isn't updating (or isn't
// drawing) must not be moved forward either.
static bool th16_bullets_held() {
	void* pause = *th16_pause_object;
	if (pause == nullptr)
		return false;
	const unsigned int flags = *(unsigned int*)((char*)pause + th16_pause_flags);
	// What the game's two bullet tasks and the walk itself stop for: the update task for the low
	// bits, the draw task for bit 2, and the walk for bit 0x400 (which skips every bullet's own
	// update while the picture is held)
	return ((flags | (flags >> 2)) & 1) != 0 || (flags & 4) != 0 || (flags & 0x400) != 0;
}

// How much of a frame's movement a bullet in this state gets (0x411E70, the switch on the state at
// +0xC72). A bullet that is in play moves a whole step, the ones that are still waiting for their
// sub-pattern to start move half of one, and one that is in neither state doesn't move at all.
// 0x200 on a bullet holds it - the game neither moves nor draws such a bullet - and a bullet in
// state 5 doesn't move at all until its timer is past 2.
static float th16_bullet_step_factor(unsigned short state, unsigned int flags, int timer) {
	switch (state) {
	case 1:
		return (flags & 0x200) != 0 ? 0.0f : 1.0f;
	case 2:
	case 3:
		return 0.5f;
	case 5:
		return timer > 2 ? 0.5f : 0.0f;
	default:
		return 0.0f;
	}
}

// Moves every bullet in play forward by t frames (t = 0 is the state the game is in) and remembers
// what it changed, so th16_bullets_undo can put it back.
static void th16_bullets_advance(float t) {
	th16_saved_bullets_count = 0;
	th16_bullets_in_play = 0;
	th16_bullets_moved = 0;

	void* manager = *th16_bullet_manager;
	if (manager == nullptr || th16_bullets_held())
		return;

	unsigned int frames_per_render = *th16_frames_per_render;
	if (frames_per_render > 2) // the game's own setting only goes to 2
		frames_per_render = 2;

	float scale = *th16_time_scale;
	if (!(scale > 0.0f) || scale > 2.0f)
		scale = 1.0f;

	const float step = t * (float)(frames_per_render + 1) * scale;

	void* node = *(void**)((char*)manager + th16_bullet_list_head);
	// The list is at most as long as the pool, and this runs with the game paused in its own frame
	// function, so it can't change under us; the bound is there to keep a list that got corrupted
	// some other way from running away with the frame.
	for (size_t guard = 0; node != nullptr && guard < th16_bullets_saved_max * 4; ++guard) {
		void* bullet = *(void**)node;
		void* next = *(void**)((char*)node + th16_bullet_list_next);
		if (bullet == nullptr)
			break;

		++th16_bullets_in_play;
		const float factor = th16_bullet_step_factor(
			*(unsigned short*)((char*)bullet + th16_bullet_state),
			*(unsigned int*)((char*)bullet + th16_bullet_flags),
			*(int*)((char*)bullet + th16_bullet_timer));
		if (factor != 0.0f && th16_saved_bullets_count < th16_bullets_saved_max) {
			float* position = (float*)((char*)bullet + th16_bullet_position);
			const float* velocity = (const float*)((char*)bullet + th16_bullet_velocity);
			Th16SavedBullet& saved = th16_saved_bullets[th16_saved_bullets_count++];
			saved.bullet = bullet;

			const float bullet_step = step * factor;
			for (int axis = 0; axis < 3; ++axis) {
				saved.position[axis] = position[axis];
				position[axis] += velocity[axis] * bullet_step;
			}
			++th16_bullets_moved;
		}

		node = next;
	}
}

// Puts the positions of every bullet th16_bullets_advance moved back, so the game's own logic sees
// the state it left behind and not the projected one.
static void th16_bullets_undo() {
	for (size_t i = 0; i < th16_saved_bullets_count; ++i) {
		const Th16SavedBullet& saved = th16_saved_bullets[i];
		float* position = (float*)((char*)saved.bullet + th16_bullet_position);
		for (int axis = 0; axis < 3; ++axis)
			position[axis] = saved.position[axis];
	}
	th16_saved_bullets_count = 0;
}

// ---------------------------------------------------------------------------
// Frame interpolation, stage 1: the player, its options and its shots
// ---------------------------------------------------------------------------
//
// The player of the stage that is running is at 0x4A6EF8 (0x441C60 builds it and puts itself
// there, 0x441A50 releases it; null in the menus and before a stage starts). Its own picture is
// the animation object at +0x14, and the render pass of the player (0x443730, the task the game
// registers with the draw chain) copies its position (+0x610) into that object's position
// (+0x5F4 = the object's +0x5E0) before drawing it, so moving the position moves the player's
// sprite the same way the bullets' position does.
//
// Everything else the player puts on screen is positioned by the update instead, so those sprites
// have to be moved themselves:
//
//   - the options (the sub-shots that sit beside the player) are two arrays of slots of 0xE4
//     bytes: 4 of them at +0x660 and 8 at +0x9F0 (0x442380 is their update, once per array from
//     0x441CF0). A slot that is up (its +0x00 is set) keeps two sprite object ids at
//     +0xB0/+0xB4 and a position in 1/128 pixel units at +0x5C, which the update walks toward the
//     player's position and then writes into the two sprites.
//   - the player's shots (the bullets it fires) are 256 slots of 0xC0 bytes at +0x1118. A live
//     slot (its +0x84 is set) keeps its sprite object id at +0x00 and its position in pixels at
//     +0x40, which the update copies into the sprite and then moves (0x4456D0).
//
// The sprites those two keep are found the way the game finds one from an id (0x46EFA0): the
// sprite manager at 0x4C0F48 holds its objects in slots of 0x614 bytes starting at +0xEC, each of
// them with the id it belongs to at +0x538 and an "in use" byte at +0x60C, and ids whose low 13
// bits are 0x1FFF are on one of the manager's two lists instead (the player's sprites never are).
// The lookup is written out here instead of called: it takes its argument on the stack, and a
// call through a function pointer is not put together with that in mind - the garbage it gets
// back hands back a pointer that is not a sprite at all (the same mistake th15 made once).
//
// The player moves by what the input says rather than by a velocity that could be read back, and
// the options and shots move by what comes out of that plus their own motion, so what each sprite
// moves by in a frame is measured instead of read: every game frame the positions are compared
// with the ones of the frame before, and the extra presentations move the sprites by that
// difference times the fraction of a frame they stand for. Something that didn't move - a held
// picture, a paused game - comes out as zero on its own.

static void** const th16_player = (void**)0x004A6EF8;
static const size_t th16_player_position = 0x610;        // the player's own position, 3 floats
static const size_t th16_player_options_a = 0x660;       // the first option array
static const size_t th16_player_options_a_count = 4;
static const size_t th16_player_options_b = 0x9F0;       // the second one
static const size_t th16_player_options_b_count = 8;
static const size_t th16_player_option_size = 0xE4;
static const size_t th16_player_option_sprite = 0xB0;    // 2 object ids
static const size_t th16_player_shots = 0x1118;          // 256 slots of 0xC0 bytes
static const size_t th16_player_shot_count = 256;
static const size_t th16_player_shot_size = 0xC0;
static const size_t th16_player_shot_live = 0x84;        // nonzero while the shot is in use
static const size_t th16_player_shot_sprite = 0x00;      // the sprite's object id
static const size_t th16_object_position = 0x5E0;        // in a sprite object, 3 floats
// Nothing on screen moves this far in one frame (the player's own speed is a few pixels), so a
// difference bigger than this is a slot that was handed to another object, not a movement
static const float th16_max_step = 48.0f;

// One thing to move for an extra presentation: the sprite object whose position is shifted. What
// the sprite moved by is measured on that same position, so the entry only has to remember it.
static const size_t th16_tracked_max = 1
	+ (th16_player_options_a_count + th16_player_options_b_count) * 2
	+ th16_player_shot_count;

struct Th16SpriteMotion {
	void* sprite;         // the sprite object it is about, null while the entry is free
	unsigned int frame;   // the frame it was last measured in
	float last_x, last_y;
	float delta_x, delta_y;
};

static Th16SpriteMotion th16_motions[th16_tracked_max];
static unsigned int th16_motion_frame = 1;
static float* th16_tracked[th16_tracked_max];
static unsigned int th16_tracked_kind[th16_tracked_max];
static size_t th16_tracked_count;
static float th16_tracked_saved[th16_tracked_max][3];

static const unsigned int th16_kind_body = 0;
static const unsigned int th16_kind_shot = 1;

// Diagnostics: the largest movement the player's own position had since the last dump
static float th16_player_movement;
// Diagnostics: how many sprites this frame couldn't be given a movement record (see
// th16_sprite_motion - it should stay 0)
static unsigned int th16_sprites_untracked;

// The sprite object an id refers to: the same lookup the game does (0x46EFA0), written out
// instead of called (see the comment above).
static void* th16_object_by_id(unsigned int id) {
	const unsigned int index = id & 0x1FFF;
	if (id == 0 || index == 0x1FFF)
		return nullptr;

	void* manager = *(void**)0x004C0F48;
	if (manager == nullptr)
		return nullptr;

	char* object = (char*)manager + 0xEC + (size_t)index * 0x614;
	if (*(unsigned char*)(object + 0x60C) == 0)
		return nullptr;
	if (*(unsigned int*)(object + 0x538) != id)
		return nullptr;
	return object;
}

// The record of a sprite, made the first time it is seen.
//
// Nothing tells a sprite that is gone from a slot that is still up, so a record can't be dropped
// when its sprite stops being drawn - which means they have to be recycled instead. Without that,
// everything a stage ever puts on screen keeps a record of its own, the table fills up, and from
// then on every sprite that isn't in it already quietly stops being moved (the player itself is
// the first to suffer when a stage is started again, since it is built at another address then).
//
// The one that was measured longest ago is the one recycled: a record that wasn't measured in the
// frame right before this one has nothing to compare against anyway. A record that is in use this
// frame is never taken, so the sprite it belongs to keeps moving with the others.
static Th16SpriteMotion* th16_sprite_motion(void* sprite) {
	Th16SpriteMotion* empty = nullptr;
	Th16SpriteMotion* oldest = nullptr;
	for (size_t i = 0; i < th16_tracked_max; ++i) {
		Th16SpriteMotion& motion = th16_motions[i];
		if (motion.sprite == sprite)
			return &motion;
		if (motion.sprite == nullptr) {
			if (empty == nullptr)
				empty = &motion;
			continue;
		}
		if (motion.frame == th16_motion_frame)
			continue;
		if (oldest == nullptr || motion.frame < oldest->frame)
			oldest = &motion;
	}

	Th16SpriteMotion* motion = empty != nullptr ? empty : oldest;
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

// Adds a sprite to what this frame moves, and works out what it moved by since the frame before
static void th16_track(float* position, unsigned int kind) {
	if (th16_tracked_count >= th16_tracked_max)
		return;
	// Two ids can name the same sprite (the game doesn't mind), and moving one twice would put it
	// twice as far out and only be able to put back one of the two
	for (size_t i = 0; i < th16_tracked_count; ++i) {
		if (th16_tracked[i] == position)
			return;
	}
	Th16SpriteMotion* motion = th16_sprite_motion(position);
	if (motion == nullptr) {
		++th16_sprites_untracked;
		return;
	}

	const float x = position[0];
	const float y = position[1];

	// Only a sprite that was measured in the frame right before this one has something to compare
	// against (one that just appeared, or that came back after the game didn't draw for a while,
	// hasn't moved as far as this can tell)
	if (motion->frame + 1 == th16_motion_frame) {
		const float dx = x - motion->last_x;
		const float dy = y - motion->last_y;
		// The pools hand their slots out again, and the object that used to be there is somewhere
		// else entirely - that difference is not something to project
		if (dx > -th16_max_step && dx < th16_max_step && dy > -th16_max_step && dy < th16_max_step) {
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
	motion->frame = th16_motion_frame;
	motion->last_x = x;
	motion->last_y = y;

	th16_tracked_kind[th16_tracked_count] = kind;
	th16_tracked[th16_tracked_count++] = position;
}

// Collects what is on screen this frame and works out what it moved by since the frame before.
// This runs once per game frame, from the game's own present (see th16_present_hook).
static void th16_player_measure() {
	th16_tracked_count = 0;
	th16_sprites_untracked = 0;
	++th16_motion_frame;

	void* player = *th16_player;
	if (player == nullptr)
		return;

	// The player itself: its own position is what the render pass reads
	th16_track((float*)((char*)player + th16_player_position), th16_kind_body);
	if (th16_tracked_count > 0) {
		const Th16SpriteMotion* motion = th16_sprite_motion(th16_tracked[0]);
		if (motion != nullptr) {
			const float x = motion->delta_x < 0.0f ? -motion->delta_x : motion->delta_x;
			const float y = motion->delta_y < 0.0f ? -motion->delta_y : motion->delta_y;
			const float magnitude = x > y ? x : y;
			if (magnitude > th16_player_movement)
				th16_player_movement = magnitude;
		}
	}

	// The options, and the shots, are only measured while they are up
	const size_t option_arrays[2][2] = {
		{ th16_player_options_a, th16_player_options_a_count },
		{ th16_player_options_b, th16_player_options_b_count },
	};
	for (size_t array = 0; array < 2; ++array) {
		for (size_t i = 0; i < option_arrays[array][1]; ++i) {
			const char* slot = (const char*)player + option_arrays[array][0] + i * th16_player_option_size;
			if (*(const int*)slot == 0)
				continue;
			for (int sprite = 0; sprite < 2; ++sprite) {
				void* object = th16_object_by_id(*(const unsigned int*)(slot + th16_player_option_sprite + sprite * 4));
				if (object == nullptr)
					continue;
				th16_track((float*)((char*)object + th16_object_position), th16_kind_body);
			}
		}
	}

	for (size_t i = 0; i < th16_player_shot_count; ++i) {
		const char* slot = (const char*)player + th16_player_shots + i * th16_player_shot_size;
		if (*(const int*)(slot + th16_player_shot_live) == 0)
			continue;
		void* object = th16_object_by_id(*(const unsigned int*)(slot + th16_player_shot_sprite));
		if (object == nullptr)
			continue;
		th16_track((float*)((char*)object + th16_object_position), th16_kind_shot);
	}
}

// Moves the player, its options and its shots t frames ahead of the state the game is in, and
// remembers what it changed
static void th16_player_advance(float t) {
	for (size_t i = 0; i < th16_tracked_count; ++i) {
		float* position = th16_tracked[i];
		for (int axis = 0; axis < 3; ++axis)
			th16_tracked_saved[i][axis] = position[axis];
		const Th16SpriteMotion* motion = th16_sprite_motion(position);
		const bool move = th16_tracked_kind[i] == th16_kind_shot
		                      ? Config::InterpolatePlayerShots
		                      : Config::InterpolatePlayerBody;
		if (move && motion != nullptr) {
			position[0] += motion->delta_x * t;
			position[1] += motion->delta_y * t;
		}
	}
}

static void th16_player_undo() {
	for (size_t i = 0; i < th16_tracked_count; ++i) {
		float* position = th16_tracked[i];
		for (int axis = 0; axis < 3; ++axis)
			position[axis] = th16_tracked_saved[i][axis];
	}
}

// ---------------------------------------------------------------------------
// Frame interpolation, stage 1: the straight lasers
// ---------------------------------------------------------------------------
//
// The lasers of a stage live in a manager of their own (0x4A6EE0, 0x610 bytes, built by
// 0x4314A0 and opened for a stage by 0x431330 with bullet.anm). It is a task like the bullets':
// its update (the thunk 0x4316B0, which is also where the game's pause object at 0x4A6DD4 comes
// in) walks the list whose first laser is at manager+0x14 and whose links are the object's own
// +0x08 (next) and +0x04 (previous), calling each object's virtual update at vtable+0x10 and its
// finish at vtable+0x18; the draw (0x431720) walks the same list and calls vtable+0x14, skipping
// the objects whose +0x10 says they are done.
//
// The line laser's draw (0x433720) builds its picture out of three of its own fields: the
// position at +0x54, the angle at +0x6C and the length at +0x70. It copies the position into the
// sprite it draws at +0x92C and puts the far end of the beam at position + polar(angle, length)
// (0x43AD00) into the one at +0x1524, so moving those fields moves the laser - the same fields
// th15 has.
//
// Each class of straight laser keeps what it moves by in a different place, so the layout is
// picked by vtable along with which classes are straight at all (the same table th15 has, and the
// same offsets, since the classes are the same ones):
//
//   LaserLineInf (vtable 0x492424)      update 0x432F40: velocity 0x60/0x64/0x68, the length
//                                       grows by the rate at 0x78 up to 0x5E4, and it does not
//                                       turn on its own.
//   LaserInfiniteInf (vtable 0x4923B8)  update 0x4352F0: velocity 0x5E0/0x5E4/0x5E8, the angle
//                                       turns by 0x5F0 and the length grows by the same 0x78 up
//                                       to 0x5F4.
//
// The beam class (vtable 0x49234C) is deliberately not in the table: its own update (0x43AC00) is
// a stub that just returns 0, so it never moves and there is nothing to project for it. The curve
// class (0x4922E0) is left out as the requirements ask. Anything else that turns up in the list
// (the manager's own root entry, for one) is skipped by only ever looking at objects with one of
// the two vtables above.

static void** const th16_laser_manager = (void**)0x004A6EE0;
static const size_t th16_laser_list = 0x14;      // the first laser of the list
static const size_t th16_laser_next = 0x08;      // in a laser: the next one
static const size_t th16_laser_position = 0x54;  // where it starts, 3 floats
static const size_t th16_laser_angle = 0x6C;     // the direction it points in, radians
static const size_t th16_laser_length = 0x70;    // how far it reaches

struct Th16LaserLayout {
	const void* vtable;
	size_t velocity;            // 3 floats
	size_t angle_speed;         // 0 for a class that doesn't turn
	size_t length_speed;
	size_t length_limit;
	size_t length_limit_subtract;
};

static const Th16LaserLayout th16_laser_layouts[] = {
	{ (const void*)0x00492424, 0x60,  0,     0x78, 0x5E4, 0 },
	{ (const void*)0x004923B8, 0x5E0, 0x5F0, 0x78, 0x5F4, 0 },
};

static const size_t th16_lasers_max = 0x400 + 1; // the manager's own limit, plus its root entry

struct Th16LaserMoved {
	void* laser;
	float position[3];
	float angle, length;
};

static Th16LaserMoved th16_lasers_moved[th16_lasers_max];
static size_t th16_lasers_moved_count;
static unsigned int th16_lasers_seen;
static unsigned int th16_lasers_projected;

// The layout of the class this laser is, or null if it isn't one of the straight ones
static const Th16LaserLayout* th16_laser_layout(const void* laser) {
	const void* vtable = *(const void* const*)laser;
	for (size_t i = 0; i < sizeof(th16_laser_layouts) / sizeof(th16_laser_layouts[0]); ++i) {
		if (vtable == th16_laser_layouts[i].vtable)
			return &th16_laser_layouts[i];
	}
	return nullptr;
}

// Moves the straight lasers t frames ahead of the state the game is in, and remembers what it
// changed. Each laser is put where the velocity it carries right now would have it t frames from
// here: the game's own update adds velocity * the global scale to the position once per frame, and
// this is that same step for the fraction of a frame this presentation stands for.
static void th16_lasers_advance(float t) {
	th16_lasers_moved_count = 0;
	th16_lasers_seen = 0;
	th16_lasers_projected = 0;

	void* manager = *th16_laser_manager;
	if (manager == nullptr || th16_bullets_held())
		return;

	unsigned int frames_per_render = *th16_frames_per_render;
	if (frames_per_render > 2) // the game's own setting only goes to 2
		frames_per_render = 2;

	float scale = *th16_time_scale;
	if (!(scale > 0.0f) || scale > 2.0f)
		scale = 1.0f;

	const float step = t * (float)(frames_per_render + 1) * scale;

	void* laser = *(void**)((char*)manager + th16_laser_list);
	// The list is the game's own and this runs where nothing can change it; the bound is only so a
	// list that got broken some other way can't run away with the frame
	for (size_t guard = 0; laser != nullptr && guard < th16_lasers_max * 4; ++guard) {
		void* next = *(void**)((char*)laser + th16_laser_next);
		const Th16LaserLayout* layout = th16_laser_layout(laser);
		if (layout != nullptr && th16_lasers_moved_count < th16_lasers_max) {
			++th16_lasers_seen;
			Th16LaserMoved& moved = th16_lasers_moved[th16_lasers_moved_count++];
			moved.laser = laser;

			float* position = (float*)((char*)laser + th16_laser_position);
			float* angle = (float*)((char*)laser + th16_laser_angle);
			float* length = (float*)((char*)laser + th16_laser_length);
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
				float limit = *(const float*)((char*)laser + layout->length_limit);
				if (layout->length_limit_subtract != 0)
					limit -= *(const float*)((char*)laser + layout->length_limit_subtract);
				*length += *(const float*)((char*)laser + layout->length_speed) * step;
				if (limit > 0.0f && *length > limit)
					*length = limit;
			}
			++th16_lasers_projected;
		}
		laser = next;
	}
}

static void th16_lasers_undo() {
	for (size_t i = 0; i < th16_lasers_moved_count; ++i) {
		const Th16LaserMoved& moved = th16_lasers_moved[i];
		float* position = (float*)((char*)moved.laser + th16_laser_position);
		for (int axis = 0; axis < 3; ++axis)
			position[axis] = moved.position[axis];
		*(float*)((char*)moved.laser + th16_laser_angle) = moved.angle;
		*(float*)((char*)moved.laser + th16_laser_length) = moved.length;
	}
	th16_lasers_moved_count = 0;
}

// Diagnostics: press U to write the presentation timing of the last frames to oilp_diag.txt
static const unsigned int th16_diag_frames_max = 300;
static const unsigned int th16_diag_presents_max = 8;
struct Th16DiagFrame {
	LARGE_INTEGER base;                                      // the game's own present of the frame
	unsigned int presents;
	LARGE_INTEGER present_time[th16_diag_presents_max];      // when each presentation happened
};
static Th16DiagFrame th16_diag_frames[th16_diag_frames_max];
static unsigned int th16_diag_count;
static bool th16_diag_key_down;

static LARGE_INTEGER th16_perf_freq;

static void th16_diag_dump() {
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

	unsigned int count = th16_diag_count < th16_diag_frames_max ? th16_diag_count : th16_diag_frames_max;
	unsigned int start = th16_diag_count < th16_diag_frames_max ? 0 : th16_diag_count % th16_diag_frames_max;

	fprintf(file, "oilp frame interpolation diagnostics\n");
	fprintf(file, "logic frame rate: %.2f, configured presentation rate: %.2f\n",
		Limiter::game_fps, Limiter::PresentRate());
	fprintf(file, "frames recorded: %u\n", count);
	fprintf(file, "bullets: %u in play, %u projected forward (last game frame)\n",
		th16_bullets_in_play, th16_bullets_moved);
	fprintf(file, "player: %u sprite(s) moved (the player, its options and its shots)\n",
		(unsigned int)th16_tracked_count);
	fprintf(file, "player: %u sprite(s) this frame had no movement record (should stay 0)\n",
		th16_sprites_untracked);
	fprintf(file, "player movement: up to %.2f px per frame since the last dump\n",
		th16_player_movement);
	th16_player_movement = 0.0f;
	fprintf(file, "lasers: %u straight laser(s) in play (the curve ones are left alone)\n",
		th16_lasers_seen);

	double presents = 0.0;
	double first = 0.0;
	double last = 0.0;
	for (unsigned int i = 0; i < count; ++i) {
		const Th16DiagFrame& frame = th16_diag_frames[(start + i) % th16_diag_frames_max];
		fprintf(file, "frame %4u: %u present(s)", i, frame.presents);
		for (unsigned int p = 0; p < frame.presents && p < th16_diag_presents_max; ++p)
			fprintf(file, " [%u] %.2fms", p,
				(double)(frame.present_time[p].QuadPart - frame.base.QuadPart) * 1000.0 / (double)th16_perf_freq.QuadPart);
		fprintf(file, "\n");
		presents += frame.presents;
		if (i == 0)
			first = (double)frame.base.QuadPart;
		last = (double)frame.base.QuadPart;
	}
	if (last > first)
		fprintf(file, "average: %.3f presents per frame, %.1f presentations per second\n",
			presents / (double)count, presents * (double)th16_perf_freq.QuadPart / (last - first));

	fclose(file);
	printf("Wrote oilp_diag.txt\n");
}

// One extra presentation of the frame: the same render block the game runs, once more, so the state
// the game is in is drawn and presented again, with the bullets projected t frames ahead of it -
// t = 0 is the state the game's own presentation showed, and the presentations of one frame reach
// t = 1, which is where the game's next one takes over. The tool's frame is readied again, so the
// object render below draws its overlay into this presentation the same way it does in the game's
// own one - and it reads the projected positions, so a tool that draws something on the bullets
// (thprac's hitbox display, for example) stays on them.
static void th16_extra_presentation(float t) {
	IDirect3DDevice9* device = d3d9_hooked_device();
	if (device == nullptr)
		return;

	device->BeginScene();
	// The game's own pass set the transforms before this point, and the menus change them, so put back
	// whatever the scene was drawn with (see th16_render_hook)
	if (th16_scene_transforms) {
		device->SetTransform(D3DTS_VIEW, &th16_scene_view);
		device->SetTransform(D3DTS_PROJECTION, &th16_scene_projection);
	}
	// The game's own pass set the viewport before this point, and the pause screens and the menus
	// change it, so put back whatever the scene was drawn with (see th16_render_hook)
	if (th16_scene_viewport.Width != 0)
		device->SetViewport(&th16_scene_viewport);
	// The game sets its camera up before its render block and a menu sets those up differently for
	// itself, so an extra presentation calls the game's own setup again to get the transforms above
	// plus the two values it leaves in the sprite manager that place the queue on screen.
	//
	// The queue is emptied first: the setup draws the queue once on its way in, and with nothing
	// queued that draw does nothing. The render block below empties it again anyway.
	th16_reset_sprite_queue();
	th16_camera_setup(th16_camera_object);
	th16_reset_sprite_queue();
	*th16_render_flag = 0xFF;
	th16_prepare_render(th16_render_context);
	ToolState::ReadyToDraw();
	// The objects are drawn where they will be t frames from now; the render bakes the positions
	// into the sprite queue, so they can go back right after it
	if (Config::InterpolateBulletsAndLasers) {
		th16_bullets_advance(t);
	}
	th16_player_advance(t);
	if (Config::InterpolateBulletsAndLasers) {
		th16_lasers_advance(t);
	}
	// The text list goes back to how the game's own run of the walk found it, so this presentation
	// draws the same text that one drew (see th16_text_record_count above)
	th16_text_records_restore();
	// The stage's draw callback advances its transition (see th16_stage_save); put the object back
	// afterwards so an extra pass cannot move the transition a second or third time
	th16_stage_save();
	th16_in_extra = true;
	// Put the shake back for this pass (see the comment above)
	extra_float_state_begin(&th16_shake);
	th16_render_objects();
	extra_float_state_end(&th16_shake);
	th16_in_extra = false;
	th16_stage_restore();
	th16_lasers_undo();
	th16_player_undo();
	th16_bullets_undo();
	th16_draw_sprite_queue(*th16_sprite_manager);
	device->SetTexture(0, nullptr);
	device->EndScene();

	// The extras are presented with the device's own Present, without the hooks the tools put in
	// front of it (see d3d9_present_bypassing_hooks)
	d3d9_present_bypassing_hooks();
	overlay_mark_presentation();
}

// Presents the frame the game just drew, and then the extra presentations the config asked for.
// This is what the game's own present call runs instead of: the call is replaced with a call to
// this, and the present the game sets up after it (pushing the arguments and calling the device)
// is skipped over, so the game ends up right after its own present call again, with the result in
// eax where its code expects it.
static HRESULT th16_present_hook() {
	IDirect3DDevice9* device = d3d9_hooked_device();
	if (device == nullptr)
		return E_FAIL;

	static bool freq_queried = false;
	if (!freq_queried) {
		freq_queried = true;
		QueryPerformanceFrequency(&th16_perf_freq);
	}

	LARGE_INTEGER base;
	QueryPerformanceCounter(&base);

	// The game's own update has run and it is about to put that frame on screen: this is the one
	// point per frame where the movement of the player, its options and its shots can be measured
	th16_player_measure();

	// The game's own presentation of this frame, through the device it drew with, so that the
	// hooks the user's tools have on it still see it
	HRESULT result = device->Present(nullptr, nullptr, nullptr, nullptr);
	overlay_mark_presentation();

	// The game's own work of this frame ends here as far as the overlay is concerned - everything
	// below is the patch presenting the same frame again
	Limiter::MarkFrameWorkEnd();

	// The tool has drawn its frame by now: the other half of watching for its frame state
	ToolState::WatchFrameDrawn();

	unsigned int presents = 1;
	LARGE_INTEGER present_time[th16_diag_presents_max];
	present_time[0] = base;

	double rate = Limiter::PresentRate();
	// Nothing more to present if the presentation that just happened failed (a lost device, for
	// example) - the game's own code right after this handles that
	if (SUCCEEDED(result) && rate > 0.0 && Limiter::game_fps > 0.0 && !Limiter::PresentingEveryNthFrame()) {
		static bool rate_printed = false;
		if (!rate_printed) {
			rate_printed = true;
			printf("Frame interpolation: %.1f presentations per second (%.2f per game frame)\n",
				rate, rate / Limiter::game_fps);
		}

		// How many presentations this frame gets. The game's own present is one of them, and the
		// fraction is carried over to the next frame, so a rate that isn't a multiple of the game's
		// frame rate still averages out ("*144" with 60fps logic presents 2 and 3 alternately).
		// How many presentations this frame gets, and how far apart they sit: the limiter works
		// both out (see Limiter::ExtraPresentations), carrying a rate that is not a multiple of
		// the game's frame rate over from frame to frame, so every game behaves the same way here
		LARGE_INTEGER spacing;
		unsigned int extras = Limiter::ExtraPresentations(rate, spacing);
		LARGE_INTEGER frame_wait = Limiter::FrameWait();
		if (extras > 0 && spacing.QuadPart > 0) {
			// The presentations can't run past the end of the frame either - the limiter's
			// schedule is absolute, so a frame that overruns is a frame the game falls behind
			// on. An extra presentation costs about as much as the game's own pass did, so if
			// the last ones wouldn't fit before the next frame starts they're dropped (the
			// frame is then presented fewer times, but never late).
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

				if (presents < th16_diag_presents_max)
					QueryPerformanceCounter(&present_time[presents]);
				++presents;

				// Where this presentation sits between the game's own one and the next one:
				// the frames of one game frame are spread evenly over it, so the k-th of
				// extras + 1 of them draws the state k/(extras + 1) frames along (stage 1)
				th16_extra_presentation((float)k / (float)(extras + 1));
			}
		}
	}

	// Record the frame for the diagnostics
	{
		Th16DiagFrame& frame = th16_diag_frames[th16_diag_count % th16_diag_frames_max];
		frame.base = base;
		frame.presents = presents;
		for (unsigned int p = 0; p < th16_diag_presents_max; ++p)
			frame.present_time[p] = p < presents ? present_time[p] : base;
		++th16_diag_count;
	}

	// U dumps the diagnostics
	if (GetAsyncKeyState('U') & 0x8000) {
		if (!th16_diag_key_down) {
			th16_diag_key_down = true;
			th16_diag_dump();
		}
	} else {
		th16_diag_key_down = false;
	}

	return result;
}

// The present the game sets up at each present call site: the arguments are pushed and the device
// is called, 14 bytes in all (see th16_install_patches)
static const size_t th16_present_setup_size = 14;

// Jumps over the present the game sets up after its present call site, so the code after it runs
// with the result the hook left in eax
static void th16_skip_present_setup(DWORD address) {
	BYTE patch[th16_present_setup_size];
	patch[0] = 0xE9;
	*(DWORD*)(patch + 1) = (address + th16_present_setup_size) - (address + 5);
	for (size_t i = 5; i < th16_present_setup_size; ++i)
		patch[i] = 0x90;
	patch_bytes(address, patch, sizeof(patch));
}

void th16_install_patches() {
	{
		// Skip the original frame limiter
		BYTE patch[] = { 0xEB, 0x4B };
		patch_bytes(0x0045AC9D, patch, sizeof(patch));
	}
	{
		// Force fast input latency mode
		BYTE patch[] = { 0xEB };
		patch_bytes(0x00459F72, patch, sizeof(patch)); // Skip over automatic
		patch_bytes(0x00459F87, patch, sizeof(patch)); // Skip over normal
	}
	{
		// Hook window update
		patch_call(0x00459FA3, th16_window_update_hook);
	}
	if (Config::D3D9Ex) {
		// Redirect Direct3DCreate9 call
		// IAT is being hooked, but thcrap also hooks Direct3DCreate9 in the same place
		// This should force our Direct3DCreate9 hook to be loaded no matter what
		patch_call(0x00459A84, Direct3DCreate9_hook);

		// Extra NOP is needed because we're replacing a FF 15 call, which is 6 bytes long
		BYTE patch[] = { 0x90 };
		patch_bytes(0x00459A89, patch, sizeof(patch));
	}
	if (Config::ReplaySpeedControl) {
		// Skip the original replay speed control stuff
		BYTE patch[] = { 0xEB };
		patch_bytes(0x00448E62, patch, sizeof(patch));
	}
	if (Config::Interpolation || Config::InterpolationFPS) {
		// Take over the object render, only to remember the viewport the scene is drawn with (see
		// th16_render_hook); the render itself runs exactly the way it always did
		static const DWORD render_calls[] = { 0x0045A99C, 0x0045AD9C, 0x0045AF28 };
		unsigned int render_patched = 0;
		for (DWORD address : render_calls) {
			BYTE* render_call = (BYTE*)address;
			if (render_call[0] == 0xE8) {
				patch_call(address, (void*)th16_render_hook);
				++render_patched;
			} else {
				printf("th16 render call at 0x%x doesn't look like a known one, skipping!\n", address);
			}
		}

		// The fade creation block does not check its allocation for null, so its guard branch is
		// trampolined instead (see th16_fade_create_branch)
		BYTE* fade_branch = (BYTE*)0x0040A1B2;
		if (fade_branch[0] == 0x0F && fade_branch[1] == 0x8D &&
			(DWORD)0x0040A1B8 + *(DWORD*)(fade_branch + 2) == 0x0040A2AD) {
			patch_jmp(0x0040A1B2, (void*)th16_fade_create_branch);
			BYTE nop = 0x90;
			patch_bytes(0x0040A1B7, &nop, 1);
		} else {
			printf("th16 fade branch at 0x40A1B2 doesn't look like the known one, skipping!\n");
		}

		// Take over the presents in the three frame functions. The instruction that loads the
		// device for the present is 5 bytes, which is exactly what the hook's call needs, and the
		// 14 bytes after it (the arguments and the present call itself) are skipped over - the hook
		// presents with the same device and arguments the game would have used.
		static const DWORD present_sites[] = { 0x0045ABAA, 0x0045ADF6, 0x0045AFFB };
		unsigned int present_patched = 0;
		for (DWORD address : present_sites) {
			BYTE* present_site = (BYTE*)address;
			if (present_site[0] == 0xA1 && *(DWORD*)(present_site + 1) == 0x004C10D8) {
				patch_call(address, (void*)th16_present_hook);
				th16_skip_present_setup(address + 5);
				++present_patched;
			} else {
				printf("th16 present call at 0x%x doesn't look like a known one, skipping!\n", address);
			}
		}
		printf("Frame interpolation: %u render call(s) and %u present call(s) taken over (setting %d)\n",
			render_patched, present_patched,
			Config::InterpolationFPS ? (int)Config::InterpolationFPS : Config::Interpolation);

		// The overlay shows the frame rate the frames are actually presented at, which only says
		// something (and is only higher than the game's own rate) with the interpolation on - so
		// it is only shown when that and the overlay are both on
		// (the overlay readouts are Config::ShowOverlay's business; see Limiter::Initialize)
		// ...and it goes right after the line the patch itself draws in the bottom left corner,
		// rather than at the bottom right corner of the window where it would sit on top of the
		// frame rate and slowdown readout thprac draws there (see overlay.h)
		overlay_set_present_rate_follow_text(true);
	}
}

FPSTarget th16_replay_callback() {
	if (*CReplayManager::InstancePtr && (*CReplayManager::InstancePtr)->mode == 1) {
		// TODO: Reverse engineer this struct instead of being lazy
		auto input = *(DWORD*)0x004A50B0;
		if (input & InputState::Focus)
			return FPSTarget::ReplaySlow;
		else if (input & (InputState::Skip | InputState::Shoot))
			return FPSTarget::ReplaySkip;
	}
	return FPSTarget::Game;
}
