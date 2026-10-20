// Touhou 17: Wily Beast and Weakest Creature v1.00b

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
#include "touhou17.h"

using namespace Touhou17;

CReplayManager** CReplayManager::InstancePtr = (CReplayManager**)0x004B77D8;

int __fastcall th17_window_update_hook(void* self) {
	Limiter::Tick();
	return CWindowManager__UpdateFast(self);
}

// ---------------------------------------------------------------------------
// Frame interpolation, stage 0: extra presentations
// ---------------------------------------------------------------------------
//
// th17 draws a game frame once per frame, with the same block th16 has, sitting in the frame
// function of whatever scene is running:
//
//     0x461221  mov eax, [0x4B5AE8]       the device
//     0x461229  CALL [ecx+0xA4]           BeginScene
//     0x46122F  FUN_0046D050              reset the sprite queue
//     0x461234  mov ecx, 0x4B5AE0         the render context
//     0x461239  mov [0x4B64F0], 0FFh
//     0x461243  FUN_004421C0              prepare the render (ECX = the context)
//     0x461248  FUN_004013E0              render every object (they fill the sprite queue)
//     0x461253  FUN_0046D0A0              draw the sprite queue (ECX = the sprite manager)
//     0x461264  CALL [ecx+0x104]          SetTexture(0, NULL)
//     0x461272  CALL [ecx+0xA8]           EndScene
//
// Just before its render block the frame function sets the camera up (0x4611DF pushes the index of
// the view, 0x4611E6 calls FUN_00409460 with the scene object at 0x4B5AE0): that pushes the view
// and projection transforms into the device, writes the two floats that place the sprite queue
// into the sprite manager (+0xD0/+0xD4), and sets the viewport up. (FUN_00409460 picks one of
// several camera views the scene keeps - the pointer the frame selected sits at [0x4B5AE0+0x6E8],
// each view being 0x12C bytes: +0x60 the view transform, +0xA0 the projection, +0xFC/+0x100 the
// two queue values and +0xE0 the viewport.)
//
// The block appears three times, once for the game scene and once for each of the two menu scenes:
// the renders are at 0x461248, 0x4622C4 and 0x4623FD, and the presentations - the game's own
// Present call - at 0x4620FD, 0x4622FF and 0x4624AC. Each present site is laid out identically:
// the device is loaded into eax, the arguments are pushed, then the present is called through the
// device's vtable, 14 bytes in all after the load (and all five of the arguments are pushed inside
// those 14 bytes, so skipping them leaves the stack the way the game expects).
//
// zun's engine separates the game's logic from its rendering, so the same state can be drawn again
// by running the render block again - that's what the extra presentations are. The game's own
// present is the first one, and the extra ones are spread evenly over the rest of the frame, right
// where the game would otherwise sit and wait for the next one, so the presents land on the
// display's refresh evenly.
//
// The projection of the drawn objects - moving them forward in time before the extra presentations
// - comes with stage 1. With stage 0 the extra presentations draw the state the game is already in,
// which is the "moved forward by zero frames" case, and is what the infrastructure - the pacing,
// the re-rendering, the overlays of the user's tools, the diagnostics - is validated with.

// The plain calls the render block makes into the game
static auto th17_reset_sprite_queue = (void(*)())0x0046D050;
static auto th17_draw_sprite_queue = (void(__fastcall*)(void*))0x0046D0A0; // ECX = the sprite manager
static auto th17_prepare_render = (void(__fastcall*)(void*))0x004421C0;    // ECX = the render context
static auto th17_render_objects = (void(*)())0x004013E0;                   // renders every object

static void* const th17_scene_object = (void*)0x004B5AE0;
static void** const th17_sprite_manager = (void**)0x00509A20;
// The dword the render block sets to 0xFF
static DWORD* const th17_render_flag = (DWORD*)0x004B64F0;

// Where the camera the scene is drawn with is kept, and what sits in it
static const size_t th17_camera_pointer = 0x6E8;   // in the scene: the camera view in use
static const size_t th17_camera_view = 0x60;       // in the camera: the view transform
static const size_t th17_camera_projection = 0xA0; // the projection transform
static const size_t th17_camera_queue = 0xFC;      // the two floats the sprite manager gets
static const size_t th17_camera_viewport = 0xE0;   // the viewport
static const size_t th17_sprite_queue = 0xD0;      // in the sprite manager: where the queue goes

// Where the user's tool builds its frame and where it draws it. thprac's th17 module starts its
// frame (its NewFrame, its UI code, and its EndFrame) from a hook on the return of the scene
// update at 0x4013B5, and draws the overlay it built from a hook on the return of the object
// render at 0x4014D0. The extra presentations render the scene again, which erases the overlay the
// game's own pass drew, so the tool has to draw it again for every one of them (see tool_state.h -
// its frame state is found at runtime and set again there, which draws the overlay without running
// any of the tool's own code).
static unsigned char* const th17_tool_frame_address = (unsigned char*)0x004013B5;
static unsigned char* const th17_tool_draw_address = (unsigned char*)0x004014D0;

// The viewport the scene is drawn with, and the two transforms the game sets before its render
// block (FUN_00409460 calls SetTransform with D3DTS_VIEW and D3DTS_PROJECTION), taken from the
// frame the game's own render ran in. An extra presentation runs after the whole frame has been
// drawn and a menu sets up a camera of its own, so these are put back before the frame is drawn
// again (see th17_extra_presentation).
static D3DVIEWPORT9 th17_scene_viewport = {};
static D3DMATRIX th17_scene_view = {};
static D3DMATRIX th17_scene_projection = {};
static bool th17_scene_transforms = false;

// The menus draw their text through a list of records the game keeps in the text object it holds
// at [0x4B7678]: drawing a string appends a record (0x138 bytes each, at +0xC10 - the append is
// FUN_00408190) and bumps the frame's count of them (+0x19210), and the object's own draw
// (FUN_00408560) then walks the records that count covers. The appends happen while the frame is
// being drawn, so an extra presentation that draws the same screen appends its own records after
// the ones the game's own pass left behind and draws both - the same text twice, which the alpha
// blending these screens use turns into text that looks a little denser than the game's own frame
// shows, or, where the second batch is placed differently, into a second copy of it.
//
// So the count the game's own pass started from is kept here and put back before every extra
// presentation: each one then appends its own records from the start of the list, exactly the way
// the game's own frame did, and what it leaves behind is what the game's own pass left.
static void** const th17_text_object = (void**)0x004B7678;
static const size_t th17_text_record_count = 0x19210;
static unsigned int th17_text_records;

static unsigned int th17_text_count() {
	void* text = *th17_text_object;
	return text != nullptr ? *(unsigned int*)((char*)text + th17_text_record_count) : 0;
}

static void th17_text_records_save() {
	th17_text_records = th17_text_count();
}

static void th17_text_records_restore() {
	void* text = *th17_text_object;
	if (text != nullptr)
		*(unsigned int*)((char*)text + th17_text_record_count) = th17_text_records;
}

// Sets the camera of the current scene up again the way FUN_00409460 does for the view the frame
// chose (see the comment above). The scene keeps several camera views; the one the frame is drawn
// with is the one [0x4B5AE0+0x6E8] points at, so an extra presentation can set the same state up
// without knowing which index the frame function used.
static void th17_camera_setup() {
	char* camera = *(char**)((char*)th17_scene_object + th17_camera_pointer);
	if (camera == nullptr)
		return;
	IDirect3DDevice9* device = d3d9_hooked_device();
	if (device == nullptr)
		return;

	device->SetTransform(D3DTS_VIEW, (const D3DMATRIX*)(camera + th17_camera_view));
	device->SetTransform(D3DTS_PROJECTION, (const D3DMATRIX*)(camera + th17_camera_projection));

	char* sprites = *(char**)th17_sprite_manager;
	if (sprites != nullptr) {
		*(float*)(sprites + th17_sprite_queue) = *(float*)(camera + th17_camera_queue);
		*(float*)(sprites + th17_sprite_queue + 4) = *(float*)(camera + th17_camera_queue + 4);
	}

	device->SetViewport((const D3DVIEWPORT9*)(camera + th17_camera_viewport));
}

// Remembers the viewport and transforms the scene is about to be drawn with, then lets the game's
// own render run exactly as it always did (see th17_extra_presentation for what it's for)
// The screen shake: the game keeps the offset it shakes the picture by in the Float2 at 0x4B6198 and
// zeroes it inside its own draw pass, once it has been applied (the diagnostics showed it going from
// the shake offset to 0 while the frame was drawn, and the picture an extra pass drew came out that
// many pixels away from the one the game's own pass drew - the same pattern as th08's and th20's
// shake). The value the game's pass started with is therefore put back around every extra pass.
static ExtraFloatState th17_shake = { (float*)0x004B6198, 2 };

// ---------------------------------------------------------------------------
// Frame interpolation: the stage object's transition state
// ---------------------------------------------------------------------------
//
// The stage object at [0x4B7680] (and the second slot at [0x4B767C], 0x67B8 bytes) is drawn by
// the callback at 0x40A590, which changes the object in two places:
//
//   0x40AC7C: if ([stage+0x6794] > 0) it subtracts one from the stage clear timer at
//             [stage+0x6790]
//   0x40A76E: if ([stage+0x678C] & 4 and [stage+0x6794] < 30) it allocates the 0x40 byte fade
//             object at 0x40A77D and registers its tasks
//
// The game draws once a frame, so those callbacks run once a frame. An extra presentation runs the
// render chain again, which subtracts the timer two or three times per game frame and can create
// more than one fade for the same frame. The fade object itself is fine: its own timer runs on the
// update chain, once a frame.
//
// As in th13-th16 the fade is built inline, so the allocation call at 0x40A77D is guarded:
// returning nullptr there makes the callback take its own failure path (0x40A890) and skip the
// fade, while the rest of the draw runs normally.
static const size_t th17_stage_size = 0x67B8;
static void** const th17_stage_slots[2] = { (void**)0x004B767C, (void**)0x004B7680 };
static unsigned char* th17_stage_saved[2];
static bool th17_stage_saved_valid[2];
static bool th17_in_extra;

static void th17_stage_save() {
	for (int i = 0; i < 2; ++i) {
		th17_stage_saved_valid[i] = false;
		void* stage = *th17_stage_slots[i];
		if (stage == nullptr)
			continue;
		if (th17_stage_saved[i] == nullptr)
			th17_stage_saved[i] = (unsigned char*)malloc(th17_stage_size);
		if (th17_stage_saved[i] == nullptr)
			continue;
		memcpy(th17_stage_saved[i], stage, th17_stage_size);
		th17_stage_saved_valid[i] = true;
	}
}

static void th17_stage_restore() {
	for (int i = 0; i < 2; ++i) {
		void* stage = *th17_stage_slots[i];
		if (!th17_stage_saved_valid[i] || stage == nullptr)
			continue;
		memcpy(stage, th17_stage_saved[i], th17_stage_size);
		th17_stage_saved_valid[i] = false;
	}
}

static void* (__cdecl* th17_operator_new)(size_t size) = (void*(__cdecl*)(size_t))0x0047B250;

static void* __cdecl th17_fade_alloc_guard(size_t size) {
	if (th17_in_extra)
		return nullptr;
	return th17_operator_new(size);
}

static void th17_render_hook() {
	IDirect3DDevice9* device = d3d9_hooked_device();
	if (device != nullptr) {
		device->GetViewport(&th17_scene_viewport);
		th17_scene_transforms = SUCCEEDED(device->GetTransform(D3DTS_VIEW, &th17_scene_view)) &&
			SUCCEEDED(device->GetTransform(D3DTS_PROJECTION, &th17_scene_projection));
	}

	// The tool's frame is built by now (it runs from the scene update's return, before this) but
	// not drawn yet (that happens on the return of the render below): the point to watch it from
	ToolState::WatchFrameReady(th17_tool_frame_address, th17_tool_draw_address);

	// What the game's own run of the walk finds the text list at, for the extra presentations to
	// put back (see th17_text_record_count)
	th17_text_records_save();
	extra_float_state_save(&th17_shake);

	th17_render_objects();
}

// ---------------------------------------------------------------------------
// Frame interpolation, stage 1: project the bullets forward
// ---------------------------------------------------------------------------
//
// The bullets of a stage live in the bullet manager, which is built by FUN_004147C0 and put in
// 0x4B768C (it loads bullet.anm; the stage opens it from 0x4306A6). The manager owns one pool of
// 2001 bullets (0xE88 bytes each, starting at manager+0xEC) and threads the ones in play onto a
// list of their own: its first node is at manager+0xC0, the node is part of the bullet itself
// (bullet+0x10, with the bullet at node[0] and the next node at node[4]), which is how the game's
// own update walks it (0x415770: `mov esi, [edi+0xC0h]` / `mov esi, [esi]` / `mov eax, [esi+4]`).
//
// A bullet keeps three floats of position (+0x62C) and three of velocity (+0x638). The game moves
// the position by the velocity once per frame (0x415121, the generic move at the end of the state
// switch in 0x414AE0: `position += velocity * [0x4B5918]`, where 0x4B5918 is the scale the game
// applies to every movement while the screen is full). It is the 16 bit state at +0xE50 that
// decides how much of that step a bullet gets: state 1 moves a whole step, the states 2 and 3
// scale it by the 0.5 at 0x4A3B48, and anything else doesn't move at all. A bullet whose 0x200
// flag is set is held: state 1 skips its move and the draw pass leaves it out of the layers it
// draws.
//
// Two things about the game matter here and are reproduced:
//
//   - The game only renders every frames_per_render + 1 of its updates (the byte at 0x4B5CD4,
//     which the three frame functions compare their counter against), so the step between two
//     rendered frames is that many of its update steps. (The byte is never written in this game,
//     so it stays 0, but the code keeps the general form the other games have.)
//   - While the pause screen holds the picture the bullets are neither updated nor drawn: the two
//     bullet tasks (0x415980 and 0x4159B0) and the walk itself test the stage object at 0x4B76B0
//     for that (its flags are at +0x8C). A bullet that is waiting for its sub-pattern to start is
//     moved by half a step (the states 2 and 3).
//
// The drawn sprites follow the position on their own: the bullet draw pass (0x4159B0) copies
// position into the animation object the bullet carries before handing it to the sprite queue.

// The bullet manager of the stage that is running (null while no stage is)
static void** const th17_bullet_manager = (void**)0x004B768C;
static const size_t th17_bullet_list_head = 0xC0; // in it: the first node of the bullets in play
static const size_t th17_bullet_list_next = 0x04; // in a node: the next node
static const size_t th17_bullet_position = 0x62C; // in a bullet: its position, 3 floats
static const size_t th17_bullet_velocity = 0x638; // in a bullet: what it moves by, 3 floats
static const size_t th17_bullet_state = 0xE50;    // in a bullet: what it's doing, 16 bits
static const size_t th17_bullet_timer = 0xE58;    // in a bullet: how long it has been doing it
static const size_t th17_bullet_flags = 0x20;     // in a bullet: 0x200 holds it (and hides it)
// The factor the game scales every movement with (1 unless the screen is full enough for it to
// slow down)
static float* const th17_time_scale = (float*)0x004B5918;
// How many of the game's updates it runs per rendered frame, minus one
static unsigned char* const th17_frames_per_render = (unsigned char*)0x004B5CD4;
// The stage object the bullet tasks test for the pause screen, and the flags they look at
static void** const th17_pause_object = (void**)0x004B76B0;
static const size_t th17_pause_flags = 0x8C;

// The bullets that were moved for the presentation being drawn, and the positions they had. The
// pool holds 2001 of them, so the list of the ones in play can't be longer; only the bullets that
// were saved are moved, so a list that somehow grew past the array would only cost the projection
// of the bullets at its end.
static const size_t th17_bullets_saved_max = 2001;
struct Th17SavedBullet {
	void* bullet;
	float position[3];
};
static Th17SavedBullet th17_saved_bullets[th17_bullets_saved_max];
static size_t th17_saved_bullets_count;

// What the last presentation projected, for the diagnostics (see th17_diag_dump)
static unsigned int th17_bullets_in_play;
static unsigned int th17_bullets_moved;

// Whether the pause screen has the picture held. A picture the game isn't updating (or isn't
// drawing) must not be moved forward either.
static bool th17_bullets_held() {
	void* pause = *th17_pause_object;
	if (pause == nullptr)
		return false;
	const unsigned int flags = *(unsigned int*)((char*)pause + th17_pause_flags);
	// What the game's two bullet tasks and the walk itself stop for: the update task for the low
	// bits, the draw task for bit 2, and the walk for bit 0x400 (which skips every bullet's own
	// update while the picture is held)
	return ((flags | (flags >> 2)) & 1) != 0 || (flags & 4) != 0 || (flags & 0x400) != 0;
}

// How much of a frame's movement a bullet in this state gets (0x414AE0, the switch on the state at
// +0xE50). A bullet that is in flight moves a whole step, the ones that are still waiting for
// their sub-pattern move half of one, and one that is in neither state doesn't move at all.
// 0x200 on a bullet holds it - the game neither moves nor draws such a bullet.
static float th17_bullet_step_factor(unsigned short state, unsigned int flags) {
	switch (state) {
	case 1:
		return (flags & 0x200) != 0 ? 0.0f : 1.0f;
	case 2:
	case 3:
		return 0.5f;
	default:
		return 0.0f;
	}
}

// Moves every bullet in play forward by t frames (t = 0 is the state the game is in) and remembers
// what it changed, so th17_bullets_undo can put it back.
static void th17_bullets_advance(float t) {
	th17_saved_bullets_count = 0;
	th17_bullets_in_play = 0;
	th17_bullets_moved = 0;

	void* manager = *th17_bullet_manager;
	if (manager == nullptr || th17_bullets_held())
		return;

	unsigned int frames_per_render = *th17_frames_per_render;
	if (frames_per_render > 2) // the game's own setting only goes to 2
		frames_per_render = 2;

	float scale = *th17_time_scale;
	if (!(scale > 0.0f) || scale > 2.0f)
		scale = 1.0f;

	const float step = t * (float)(frames_per_render + 1) * scale;

	void* node = *(void**)((char*)manager + th17_bullet_list_head);
	// The list is at most as long as the pool, and this runs with the game paused in its own frame
	// function, so it can't change under us; the bound is there to keep a list that got corrupted
	// some other way from running away with the frame.
	for (size_t guard = 0; node != nullptr && guard < th17_bullets_saved_max * 4; ++guard) {
		void* bullet = *(void**)node;
		void* next = *(void**)((char*)node + th17_bullet_list_next);
		if (bullet == nullptr)
			break;

		++th17_bullets_in_play;
		const float factor = th17_bullet_step_factor(
			*(unsigned short*)((char*)bullet + th17_bullet_state),
			*(unsigned int*)((char*)bullet + th17_bullet_flags));
		if (factor != 0.0f && th17_saved_bullets_count < th17_bullets_saved_max) {
			float* position = (float*)((char*)bullet + th17_bullet_position);
			const float* velocity = (const float*)((char*)bullet + th17_bullet_velocity);
			Th17SavedBullet& saved = th17_saved_bullets[th17_saved_bullets_count++];
			saved.bullet = bullet;

			const float bullet_step = step * factor;
			for (int axis = 0; axis < 3; ++axis) {
				saved.position[axis] = position[axis];
				position[axis] += velocity[axis] * bullet_step;
			}
			++th17_bullets_moved;
		}

		node = next;
	}
}

// Puts the positions of every bullet th17_bullets_advance moved back, so the game's own logic sees
// the state it left behind and not the projected one.
static void th17_bullets_undo() {
	for (size_t i = 0; i < th17_saved_bullets_count; ++i) {
		const Th17SavedBullet& saved = th17_saved_bullets[i];
		float* position = (float*)((char*)saved.bullet + th17_bullet_position);
		for (int axis = 0; axis < 3; ++axis)
			position[axis] = saved.position[axis];
	}
	th17_saved_bullets_count = 0;
}

// ---------------------------------------------------------------------------
// Frame interpolation, stage 1: the player, its beast spirits and its shots
// ---------------------------------------------------------------------------
//
// The player of the stage that is running is at 0x4B77D0 (FUN_00446D90 builds it and puts itself
// there, 0x446F76 releases it; null in the menus and before a stage starts). Its own picture is
// the animation object at +0x10, and the render pass of the player (0x448CF0, the task the game
// registers with the draw chain - FUN_004463F0 registers it with the task id 0x1D) copies its
// position (+0x610) into that object's position (+0x5F4 = the object's +0x5E4) before drawing it,
// so moving the position moves the player's sprite the same way the bullets' position does.
//
// Everything else the player puts on screen keeps a sprite object of its own and is positioned by
// the update instead, so those sprites have to be moved themselves:
//
//   - the shots the player fires are 256 slots of 0xE0 bytes at +0x1110. A live slot (its +0x8C is
//     set) keeps its sprite object id at +0x08 and a position at +0x48, which the update
//     (FUN_0044AD70) copies into that sprite (+0x5E4) before moving it.
//   - the beast spirits the player carries (the "goast": what sits beside the player and what the
//     roar summons) live in a manager of their own (0x4B7684, built by FUN_0040E420 with
//     beast.anm). Each of them is a 0x70 byte object threaded onto the manager's list (its first
//     one at [0x4B7684+0x18], the next at +0x04) which keeps the sprite object ids it is drawn
//     with at +0x10 (the spirit itself) and at +0x28/+0x2C/+0x30/+0x38/+0x44 (the effects the
//     roar puts around the player), and its update (FUN_00410AF0) writes its position (+0x18) into
//     the sprite it is drawn with each frame.
//   - the sprite the player's own shot is drawn with is kept at +0x19094, and the player's update
//     puts it right on the player each frame.
//
// The sprites those keep are found the way the game finds one from an id (FUN_00476470): the
// sprite manager at 0x509A20 holds its objects in slots of 0x618 bytes starting at +0x6EC, each of
// them with the id it belongs to at +0x538 and an "in use" byte at +0x610, and ids whose low 14
// bits are 0x3FFF are on one of the manager's two lists instead (the player's sprites never are).
// The lookup is written out here instead of called: it takes its argument on the stack, and a call
// through a function pointer is not put together with that in mind - the garbage it gets back
// hands back a pointer that is not a sprite at all (the same mistake th15 made once).
//
// The player moves by what the input says rather than by a velocity that could be read back, and
// the beast spirits and shots move by what comes out of that plus their own motion, so what each
// sprite moves by in a frame is measured instead of read: every game frame the positions are
// compared with the ones of the frame before, and the extra presentations move the sprites by that
// difference times the fraction of a frame they stand for. Something that didn't move - a held
// picture, a paused game - comes out as zero on its own.

static void** const th17_player = (void**)0x004B77D0;
static const size_t th17_player_position = 0x610;       // the player's own position, 3 floats
static const size_t th17_player_shot_sprite = 0x19094;  // the sprite its own shot is drawn with
static const size_t th17_player_shots = 0x1110;         // 256 slots of 0xE0 bytes
static const size_t th17_player_shot_count = 256;
static const size_t th17_player_shot_size = 0xE0;
static const size_t th17_player_shot_live = 0x8C;       // nonzero while the shot is in use
static const size_t th17_player_shot_sprite_id = 0x08;  // the sprite's object id
static void** const th17_beast_manager = (void**)0x004B7684;
static const size_t th17_beast_list = 0x18;             // in it: the first beast spirit
static const size_t th17_beast_next = 0x04;             // in one: the next one
static const size_t th17_beast_sprite_count = 6;        // the sprites it is drawn with
static const size_t th17_beast_sprites[th17_beast_sprite_count] = {
	0x10, // the spirit itself
	0x28, 0x2C, // the two the roar's aura is drawn with
	0x30, 0x38, // the two it puts on the player
	0x44, // the one the roar hands the player
};
static const size_t th17_object_position = 0x5E4;       // in a sprite object, 3 floats
// Nothing on screen moves this far in one frame (the player's own speed is a few pixels), so a
// difference bigger than this is a slot that was handed to another object, not a movement
static const float th17_max_step = 48.0f;

// One thing to move for an extra presentation: the sprite object whose position is shifted. What
// the sprite moved by is measured on that same position, so the entry only has to remember it.
static const size_t th17_tracked_max = 1 + 1
	+ 40 * th17_beast_sprite_count
	+ th17_player_shot_count;

struct Th17SpriteMotion {
	void* sprite;         // the sprite object it is about, null while the entry is free
	unsigned int frame;   // the frame it was last measured in
	float last_x, last_y;
	float delta_x, delta_y;
};

static Th17SpriteMotion th17_motions[th17_tracked_max];
static unsigned int th17_motion_frame = 1;
static float* th17_tracked[th17_tracked_max];
static size_t th17_tracked_count;
static float th17_tracked_saved[th17_tracked_max][3];

// Diagnostics: the largest movement the player's own position had since the last dump
static float th17_player_movement;
// Diagnostics: how many sprites this frame couldn't be given a movement record (see
// th17_sprite_motion - it should stay 0)
static unsigned int th17_sprites_untracked;

// The sprite object an id refers to: the same lookup the game does (0x476470), written out
// instead of called (see the comment above).
static void* th17_object_by_id(unsigned int id) {
	const unsigned int index = id & 0x3FFF;
	if (id == 0 || index == 0x3FFF)
		return nullptr;

	void* manager = *(void**)0x00509A20;
	if (manager == nullptr)
		return nullptr;

	char* object = (char*)manager + 0x6EC + (size_t)index * 0x618;
	if (*(unsigned char*)(object + 0x610) == 0)
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
static Th17SpriteMotion* th17_sprite_motion(void* sprite) {
	Th17SpriteMotion* empty = nullptr;
	Th17SpriteMotion* oldest = nullptr;
	for (size_t i = 0; i < th17_tracked_max; ++i) {
		Th17SpriteMotion& motion = th17_motions[i];
		if (motion.sprite == sprite)
			return &motion;
		if (motion.sprite == nullptr) {
			if (empty == nullptr)
				empty = &motion;
			continue;
		}
		if (motion.frame == th17_motion_frame)
			continue;
		if (oldest == nullptr || motion.frame < oldest->frame)
			oldest = &motion;
	}

	Th17SpriteMotion* motion = empty != nullptr ? empty : oldest;
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
static void th17_track(float* position) {
	if (th17_tracked_count >= th17_tracked_max)
		return;
	// Two ids can name the same sprite (the game doesn't mind), and moving one twice would put it
	// twice as far out and only be able to put back one of the two
	for (size_t i = 0; i < th17_tracked_count; ++i) {
		if (th17_tracked[i] == position)
			return;
	}
	Th17SpriteMotion* motion = th17_sprite_motion(position);
	if (motion == nullptr) {
		++th17_sprites_untracked;
		return;
	}

	const float x = position[0];
	const float y = position[1];

	// Only a sprite that was measured in the frame right before this one has something to compare
	// against (one that just appeared, or that came back after the game didn't draw for a while,
	// hasn't moved as far as this can tell)
	if (motion->frame + 1 == th17_motion_frame) {
		const float dx = x - motion->last_x;
		const float dy = y - motion->last_y;
		// The pools hand their slots out again, and the object that used to be there is somewhere
		// else entirely - that difference is not something to project
		if (dx > -th17_max_step && dx < th17_max_step && dy > -th17_max_step && dy < th17_max_step) {
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
	motion->frame = th17_motion_frame;
	motion->last_x = x;
	motion->last_y = y;

	th17_tracked[th17_tracked_count++] = position;
}

// Collects what is on screen this frame and works out what it moved by since the frame before.
// This runs once per game frame, from the game's own present (see th17_present_hook).
static void th17_player_measure() {
	th17_tracked_count = 0;
	th17_sprites_untracked = 0;
	++th17_motion_frame;

	void* player = *th17_player;
	if (player == nullptr)
		return;

	// The player itself: its own position is what the render pass reads
	th17_track((float*)((char*)player + th17_player_position));
	if (th17_tracked_count > 0) {
		const Th17SpriteMotion* motion = th17_sprite_motion(th17_tracked[0]);
		if (motion != nullptr) {
			const float x = motion->delta_x < 0.0f ? -motion->delta_x : motion->delta_x;
			const float y = motion->delta_y < 0.0f ? -motion->delta_y : motion->delta_y;
			const float magnitude = x > y ? x : y;
			if (magnitude > th17_player_movement)
				th17_player_movement = magnitude;
		}
	}

	// The sprite the player's own shot is drawn with: the update puts it on the player
	{
		void* object = th17_object_by_id(*(unsigned int*)((char*)player + th17_player_shot_sprite));
		if (object != nullptr)
			th17_track((float*)((char*)object + th17_object_position));
	}

	// The beast spirits and the sprites the roar draws with them, which sit on the player too
	void* manager = *th17_beast_manager;
	if (manager != nullptr) {
		void* beast = *(void**)((char*)manager + th17_beast_list);
		for (size_t guard = 0; beast != nullptr && guard < 64; ++guard) {
			for (size_t i = 0; i < th17_beast_sprite_count; ++i) {
				void* object = th17_object_by_id(*(unsigned int*)((char*)beast + th17_beast_sprites[i]));
				if (object != nullptr)
					th17_track((float*)((char*)object + th17_object_position));
			}
			beast = *(void**)((char*)beast + th17_beast_next);
		}
	}

	// The shots, which are only measured while they are up
	for (size_t i = 0; i < th17_player_shot_count; ++i) {
		const char* slot = (const char*)player + th17_player_shots + i * th17_player_shot_size;
		if (*(const int*)(slot + th17_player_shot_live) == 0)
			continue;
		void* object = th17_object_by_id(*(const unsigned int*)(slot + th17_player_shot_sprite_id));
		if (object == nullptr)
			continue;
		th17_track((float*)((char*)object + th17_object_position));
	}
}

// Moves the player, its beast spirits and its shots t frames ahead of the state the game is in, and
// remembers what it changed
static void th17_player_advance(float t) {
	for (size_t i = 0; i < th17_tracked_count; ++i) {
		float* position = th17_tracked[i];
		for (int axis = 0; axis < 3; ++axis)
			th17_tracked_saved[i][axis] = position[axis];
		const Th17SpriteMotion* motion = th17_sprite_motion(position);
		if (motion != nullptr) {
			position[0] += motion->delta_x * t;
			position[1] += motion->delta_y * t;
		}
	}
}

static void th17_player_undo() {
	for (size_t i = 0; i < th17_tracked_count; ++i) {
		float* position = th17_tracked[i];
		for (int axis = 0; axis < 3; ++axis)
			position[axis] = th17_tracked_saved[i][axis];
	}
}

// ---------------------------------------------------------------------------
// Frame interpolation, stage 1: the straight lasers
// ---------------------------------------------------------------------------
//
// The lasers of a stage live in a manager of their own (0x4B76BC, 0x610 bytes, put there by the
// stage's setup around 0x430701 next to the bullet manager and released at 0x4353D6). It is a task
// like the bullets': its update (the thunk 0x435510, which is where the game's pause object at
// 0x4B76B0 comes in) walks the list whose first laser is at manager+0x14 and whose next is the
// object's own +0x08, calling each object's virtual update at vtable+0x10 and its finish at
// vtable+0x18; the draw (0x435580) walks the same list and calls vtable+0x14, skipping the objects
// whose +0x10 says they are done.
//
// The line laser's draw (0x437A40) builds its picture out of three of its own fields: the position
// at +0x54, the angle at +0x6C and the length at +0x70. It copies the position into the sprite it
// draws at +0x958 and puts the far end of the beam at position + polar(angle, length) (0x4405A0),
// so moving those fields moves the laser - the same fields th15 and th16 have.
//
// Each class of straight laser keeps what it moves by in a different place, so the layout is
// picked by vtable along with which classes are straight at all (the same table th15 and th16
// have, and the same offsets, since the classes are the same ones):
//
//   LaserLineInf (vtable 0x4A1924)      update 0x437060: velocity 0x60/0x64/0x68, the length
//                                       grows by the rate at 0x78 up to 0x5E4, and it does not
//                                       turn on its own.
//   LaserInfiniteInf (vtable 0x4A18B8)  update 0x439B20: velocity 0x5E0/0x5E4/0x5E8, the angle
//                                       turns by 0x5F0 and the length grows by the same 0x78 up
//                                       to 0x5F4.
//
// The beam class (vtable 0x4A184C) is deliberately not in the table: its own update (0x440520) is
// a stub that just returns 0, so it never moves and there is nothing to project for it. The curve
// class (vtable 0x4A17E0) is left out as the requirements ask.

static void** const th17_laser_manager = (void**)0x004B76BC;
static const size_t th17_laser_list = 0x14;      // the first laser of the list
static const size_t th17_laser_next = 0x08;      // in a laser: the next one
static const size_t th17_laser_position = 0x54;  // where it starts, 3 floats
static const size_t th17_laser_angle = 0x6C;     // the direction it points in, radians
static const size_t th17_laser_length = 0x70;    // how far it reaches

struct Th17LaserLayout {
	const void* vtable;
	size_t velocity;            // 3 floats
	size_t angle_speed;         // 0 for a class that doesn't turn
	size_t length_speed;
	size_t length_limit;
	size_t length_limit_subtract;
};

static const Th17LaserLayout th17_laser_layouts[] = {
	{ (const void*)0x004A1924, 0x60,  0,     0x78, 0x5E4, 0 },
	{ (const void*)0x004A18B8, 0x5E0, 0x5F0, 0x78, 0x5F4, 0 },
};

static const size_t th17_lasers_max = 0x400 + 1; // the manager's own limit, plus its root entry

struct Th17LaserMoved {
	void* laser;
	float position[3];
	float angle, length;
};

static Th17LaserMoved th17_lasers_moved[th17_lasers_max];
static size_t th17_lasers_moved_count;
static unsigned int th17_lasers_seen;
static unsigned int th17_lasers_projected;

// The layout of the class this laser is, or null if it isn't one of the straight ones
static const Th17LaserLayout* th17_laser_layout(const void* laser) {
	const void* vtable = *(const void* const*)laser;
	for (size_t i = 0; i < sizeof(th17_laser_layouts) / sizeof(th17_laser_layouts[0]); ++i) {
		if (vtable == th17_laser_layouts[i].vtable)
			return &th17_laser_layouts[i];
	}
	return nullptr;
}

// Moves the straight lasers t frames ahead of the state the game is in, and remembers what it
// changed. Each laser is put where the velocity it carries right now would have it t frames from
// here: the game's own update adds velocity * the global scale to the position once per frame, and
// this is that same step for the fraction of a frame this presentation stands for.
static void th17_lasers_advance(float t) {
	th17_lasers_moved_count = 0;
	th17_lasers_seen = 0;
	th17_lasers_projected = 0;

	void* manager = *th17_laser_manager;
	if (manager == nullptr || th17_bullets_held())
		return;

	unsigned int frames_per_render = *th17_frames_per_render;
	if (frames_per_render > 2) // the game's own setting only goes to 2
		frames_per_render = 2;

	float scale = *th17_time_scale;
	if (!(scale > 0.0f) || scale > 2.0f)
		scale = 1.0f;

	const float step = t * (float)(frames_per_render + 1) * scale;

	void* laser = *(void**)((char*)manager + th17_laser_list);
	// The list is the game's own and this runs where nothing can change it; the bound is only so a
	// list that got broken some other way can't run away with the frame
	for (size_t guard = 0; laser != nullptr && guard < th17_lasers_max * 4; ++guard) {
		void* next = *(void**)((char*)laser + th17_laser_next);
		const Th17LaserLayout* layout = th17_laser_layout(laser);
		if (layout != nullptr && th17_lasers_moved_count < th17_lasers_max) {
			++th17_lasers_seen;
			Th17LaserMoved& moved = th17_lasers_moved[th17_lasers_moved_count++];
			moved.laser = laser;

			float* position = (float*)((char*)laser + th17_laser_position);
			float* angle = (float*)((char*)laser + th17_laser_angle);
			float* length = (float*)((char*)laser + th17_laser_length);
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
			++th17_lasers_projected;
		}
		laser = next;
	}
}

static void th17_lasers_undo() {
	for (size_t i = 0; i < th17_lasers_moved_count; ++i) {
		const Th17LaserMoved& moved = th17_lasers_moved[i];
		float* position = (float*)((char*)moved.laser + th17_laser_position);
		for (int axis = 0; axis < 3; ++axis)
			position[axis] = moved.position[axis];
		*(float*)((char*)moved.laser + th17_laser_angle) = moved.angle;
		*(float*)((char*)moved.laser + th17_laser_length) = moved.length;
	}
	th17_lasers_moved_count = 0;
}

// Diagnostics: press U to write the presentation timing of the last frames to oilp_diag.txt
static const unsigned int th17_diag_frames_max = 300;
static const unsigned int th17_diag_presents_max = 8;
struct Th17DiagFrame {
	LARGE_INTEGER base;                                      // the game's own present of the frame
	unsigned int presents;
	LARGE_INTEGER present_time[th17_diag_presents_max];      // when each presentation happened
};
static Th17DiagFrame th17_diag_frames[th17_diag_frames_max];
static unsigned int th17_diag_count;
static bool th17_diag_key_down;

static LARGE_INTEGER th17_perf_freq;

static void th17_diag_dump() {
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

	unsigned int count = th17_diag_count < th17_diag_frames_max ? th17_diag_count : th17_diag_frames_max;
	unsigned int start = th17_diag_count < th17_diag_frames_max ? 0 : th17_diag_count % th17_diag_frames_max;

	fprintf(file, "oilp frame interpolation diagnostics\n");
	fprintf(file, "logic frame rate: %.2f, configured presentation rate: %.2f\n",
		Limiter::game_fps, Limiter::PresentRate());
	fprintf(file, "frames recorded: %u\n", count);
	fprintf(file, "bullets: %u in play, %u projected forward (last game frame)\n",
		th17_bullets_in_play, th17_bullets_moved);
	fprintf(file, "player: %u sprite(s) moved (the player, its beast spirits and its shots)\n",
		(unsigned int)th17_tracked_count);
	fprintf(file, "player: %u sprite(s) this frame had no movement record (should stay 0)\n",
		th17_sprites_untracked);
	fprintf(file, "player movement: up to %.2f px per frame since the last dump\n",
		th17_player_movement);
	th17_player_movement = 0.0f;
	fprintf(file, "lasers: %u straight laser(s) in play (the curve ones are left alone)\n",
		th17_lasers_seen);

	double presents = 0.0;
	double first = 0.0;
	double last = 0.0;
	for (unsigned int i = 0; i < count; ++i) {
		const Th17DiagFrame& frame = th17_diag_frames[(start + i) % th17_diag_frames_max];
		fprintf(file, "frame %4u: %u present(s)", i, frame.presents);
		for (unsigned int p = 0; p < frame.presents && p < th17_diag_presents_max; ++p)
			fprintf(file, " [%u] %.2fms", p,
				(double)(frame.present_time[p].QuadPart - frame.base.QuadPart) * 1000.0 / (double)th17_perf_freq.QuadPart);
		fprintf(file, "\n");
		presents += frame.presents;
		if (i == 0)
			first = (double)frame.base.QuadPart;
		last = (double)frame.base.QuadPart;
	}
	if (last > first)
		fprintf(file, "average: %.3f presents per frame, %.1f presentations per second\n",
			presents / (double)count, presents * (double)th17_perf_freq.QuadPart / (last - first));

	fclose(file);
	printf("Wrote oilp_diag.txt\n");
}

// One extra presentation of the frame: the same render block the game runs, once more, so the state
// the game is in is drawn and presented again, with the drawn objects projected t frames ahead of
// it (stage 1) - t = 0 is the state the game's own presentation showed, and the presentations of
// one frame reach t = 1, which is where the game's next one takes over. The tool's frame is readied
// again, so the object render below draws its overlay into this presentation the same way it does
// in the game's own one.
static void th17_extra_presentation(float t) {
	IDirect3DDevice9* device = d3d9_hooked_device();
	if (device == nullptr)
		return;

	device->BeginScene();
	// The game's own pass set the transforms before this point, and the menus change them, so put
	// back whatever the scene was drawn with (see th17_render_hook)
	if (th17_scene_transforms) {
		device->SetTransform(D3DTS_VIEW, &th17_scene_view);
		device->SetTransform(D3DTS_PROJECTION, &th17_scene_projection);
	}
	// The game's own pass set the viewport before this point, and the pause screens and the menus
	// change it, so put back whatever the scene was drawn with (see th17_render_hook)
	if (th17_scene_viewport.Width != 0)
		device->SetViewport(&th17_scene_viewport);
	// The game sets its camera up before its render block and a menu sets those up differently for
	// itself, so an extra presentation calls the game's own setup again to get the transforms above
	// plus the two values it leaves in the sprite manager that place the queue on screen.
	//
	// The queue is emptied first: the setup draws the queue once on its way in, and with nothing
	// queued that draw does nothing. The render block below empties it again anyway.
	th17_reset_sprite_queue();
	th17_camera_setup();
	th17_reset_sprite_queue();
	*th17_render_flag = 0xFF;
	th17_prepare_render(th17_scene_object);
	ToolState::ReadyToDraw();
	// The objects are drawn where they will be t frames from now; the render bakes the positions
	// into the sprite queue, so they can go back right after it
	th17_bullets_advance(t);
	th17_player_advance(t);
	th17_lasers_advance(t);
	// The text list goes back to how the game's own run of the walk found it, so this presentation
	// draws the same text that one drew (see th17_text_record_count above)
	th17_text_records_restore();
	// The stage's draw callback advances its transition (see th17_stage_save); put the object back
	// afterwards so an extra pass cannot move the transition a second or third time
	th17_stage_save();
	th17_in_extra = true;
	// Put the shake back for this pass (see the comment above)
	extra_float_state_begin(&th17_shake);
	th17_render_objects();
	extra_float_state_end(&th17_shake);
	th17_in_extra = false;
	th17_stage_restore();
	th17_lasers_undo();
	th17_player_undo();
	th17_bullets_undo();
	th17_draw_sprite_queue(*th17_sprite_manager);
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
static HRESULT th17_present_hook() {
	IDirect3DDevice9* device = d3d9_hooked_device();
	if (device == nullptr)
		return E_FAIL;

	static bool freq_queried = false;
	if (!freq_queried) {
		freq_queried = true;
		QueryPerformanceFrequency(&th17_perf_freq);
	}

	LARGE_INTEGER base;
	QueryPerformanceCounter(&base);

	// The game's own update has run and it is about to put that frame on screen: this is the one
	// point per frame where the movement of the player, its beast spirits and its shots can be
	// measured
	th17_player_measure();

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
	LARGE_INTEGER present_time[th17_diag_presents_max];
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

				if (presents < th17_diag_presents_max)
					QueryPerformanceCounter(&present_time[presents]);
				++presents;

				// Where this presentation sits between the game's own one and the next one:
				// the frames of one game frame are spread evenly over it, so the k-th of
				// extras + 1 of them draws the state k/(extras + 1) frames along (stage 1)
				th17_extra_presentation((float)k / (float)(extras + 1));
			}
		}
	}

	// Record the frame for the diagnostics
	{
		Th17DiagFrame& frame = th17_diag_frames[th17_diag_count % th17_diag_frames_max];
		frame.base = base;
		frame.presents = presents;
		for (unsigned int p = 0; p < th17_diag_presents_max; ++p)
			frame.present_time[p] = p < presents ? present_time[p] : base;
		++th17_diag_count;
	}

	// U dumps the diagnostics
	if (GetAsyncKeyState('U') & 0x8000) {
		if (!th17_diag_key_down) {
			th17_diag_key_down = true;
			th17_diag_dump();
		}
	} else {
		th17_diag_key_down = false;
	}

	return result;
}

// The present the game sets up at each present call site: the arguments are pushed and the device
// is called, 14 bytes in all (see th17_install_patches)
static const size_t th17_present_setup_size = 14;

// Jumps over the present the game sets up after its present call site, so the code after it runs
// with the result the hook left in eax
static void th17_skip_present_setup(DWORD address) {
	BYTE patch[th17_present_setup_size];
	patch[0] = 0xE9;
	*(DWORD*)(patch + 1) = (address + th17_present_setup_size) - (address + 5);
	for (size_t i = 5; i < th17_present_setup_size; ++i)
		patch[i] = 0x90;
	patch_bytes(address, patch, sizeof(patch));
}

void th17_install_patches() {
	{
		// Skip the original frame limiter
		BYTE patch[] = { 0xEB, 0x53 };
		patch_bytes(0x004621E5, patch, sizeof(patch));
	}
	{
		// Force fast input latency mode
		BYTE patch[] = { 0xE9, 0x2F, 0x01, 0x00, 0x00, 0x90 };
		patch_bytes(0x0046119E, patch, sizeof(patch));
	}
	{
		// Hook window update
		patch_call(0x004612D7, th17_window_update_hook);
	}
	if (Config::D3D9Ex) {
		// Redirect Direct3DCreate9 call
		// IAT is being hooked, but thcrap also hooks Direct3DCreate9 in the same place
		// This should force our Direct3DCreate9 hook to be loaded no matter what
		patch_call(0x00460F1C, Direct3DCreate9_hook);

		// Extra NOP is needed because we're replacing a FF 15 call, which is 6 bytes long
		BYTE patch[] = { 0x90 };
		patch_bytes(0x00460F21, patch, sizeof(patch));
	}
	if (Config::ReplaySpeedControl) {
		// Skip the original replay speed control stuff
		BYTE patch[] = { 0x90, 0x90 };
		patch_bytes(0x0044E633, patch, sizeof(patch));
	}
	if (Config::Interpolation || Config::InterpolationFPS) {
		// Take over the object render, only to remember the viewport the scene is drawn with (see
		// th17_render_hook); the render itself runs exactly the way it always did
		static const DWORD render_calls[] = { 0x00461248, 0x004622C4, 0x004623FD };
		unsigned int render_patched = 0;
		for (DWORD address : render_calls) {
			BYTE* render_call = (BYTE*)address;
			if (render_call[0] == 0xE8) {
				patch_call(address, (void*)th17_render_hook);
				++render_patched;
			} else {
				printf("th17 render call at 0x%x doesn't look like a known one, skipping!\n", address);
			}
		}

		// The stage's draw callback allocates the stage clear fade at 0x40A77D; an extra
		// presentation must skip that (see th17_fade_alloc_guard)
		BYTE* fade_call = (BYTE*)0x0040A77D;
		if (fade_call[0] == 0xE8 &&
			(DWORD)0x0040A782 + *(DWORD*)(fade_call + 1) == 0x0047B250) {
			patch_call(0x0040A77D, (void*)th17_fade_alloc_guard);
		} else {
			printf("th17 fade allocation at 0x40A77D doesn't look like the known one, skipping!\n");
		}

		// Take over the presents in the three frame functions. The instruction that loads the
		// device for the present is 5 bytes, which is exactly what the hook's call needs, and the
		// 14 bytes after it (the arguments and the present call itself) are skipped over - the hook
		// presents with the same device and arguments the game would have used.
		static const DWORD present_sites[] = { 0x004620FD, 0x004622FF, 0x004624AC };
		unsigned int present_patched = 0;
		for (DWORD address : present_sites) {
			BYTE* present_site = (BYTE*)address;
			if (present_site[0] == 0xA1 && *(DWORD*)(present_site + 1) == 0x004B5AE8) {
				patch_call(address, (void*)th17_present_hook);
				th17_skip_present_setup(address + 5);
				++present_patched;
			} else {
				printf("th17 present call at 0x%x doesn't look like a known one, skipping!\n", address);
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

FPSTarget th17_replay_callback() {
	if (*CReplayManager::InstancePtr && (*CReplayManager::InstancePtr)->mode == 1) {
		// TODO: Reverse engineer this struct instead of being lazy
		auto input = *(DWORD*)0x004B3230;
		if (input & InputState::Focus)
			return FPSTarget::ReplaySlow;
		else if (input & (InputState::Skip | InputState::Shoot))
			return FPSTarget::ReplaySkip;
	}
	return FPSTarget::Game;
}
