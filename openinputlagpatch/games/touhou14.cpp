// Touhou 14: Double Dealing Character v1.00b

#include <stdio.h>
#include "patch_util.h"
#include "limiter.h"
#include "d3d9_hook.h"
#include "config.h"
#include "overlay.h"
#include "tool_state.h"
#include "touhou14.h"

using namespace Touhou14;

CReplayManager** CReplayManager::InstancePtr = (CReplayManager**)0x004DB688;

int __fastcall th14_window_update_hook(void* self) {
	Limiter::Tick();
	return CWindowManager__UpdateFast(self);
}

// ---------------------------------------------------------------------------
// Frame interpolation, stage 0: extra presentations
// ---------------------------------------------------------------------------
//
// th14 is the same engine as th15 and draws a game frame the same way, with a block that sits in
// the frame function of the scene that's running:
//
//     0x46A43E  CALL [device+0xA4]        BeginScene
//     0x46A444  FUN_00475E60              reset the sprite queue
//     0x46A449  mov ecx, 0x4D8F60
//     0x46A44E  [0x4D9960] = 0xFF
//     0x46A458  FUN_00445940              prepare the render context (ECX = 0x4D8F60)
//     0x46A45D  FUN_004013B0              render every object (they fill the sprite queue)
//     0x46A462  mov ecx, [0x4F56CC]
//     0x46A468  FUN_00475EB0              draw the sprite queue
//     0x46A479  CALL [device+0x104]       SetTexture(0, NULL)
//     0x46A487  CALL [device+0xA8]        EndScene
//
// and presents later in the same frame, with the device loaded at 0x46A67A (a 5 byte
// `mov eax, ds:[4D8F68]`) and the present itself at 0x46A68A (CALL [ecx+44h]).
//
// Both blocks appear three times, once for the game scene and once for each of the two menu
// scenes: the renders are at 0x46A45D, 0x46A86D and 0x46AA0C, the presents at 0x46A67A, 0x46A8D6
// and 0x46AAEC, and each present is laid out identically (the device is loaded into eax, the four
// arguments are pushed, then the present is called). The 5 byte load is exactly the length of a
// hooked call, and the 14 bytes after it (the arguments and the present call itself) are skipped
// over, exactly as in th15.
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

// ---------------------------------------------------------------------------
// Frame interpolation, stage 1: project the bullets forward
// ---------------------------------------------------------------------------
//
// The bullets of a stage live in the bullet manager, which is created when a stage starts
// (0x416510 allocates it and 0x416110 fills it in, loading bullet.anm for it) and released when it
// ends; 0x4DB530 holds it and is zero while no stage is running. The manager is the same object
// th15 has: a pool of 2001 bullets of 0x13F4 bytes each, allocated up front (0x416510) and
// threaded onto a free list, plus a second list of the bullets that are in play.
//
// A bullet that is in play is entered into the second list (whose head is at manager+0x80) with a
// node that is part of the bullet itself, and the game's own update walks it that way (0x417080):
// the node is at bullet+0x10, the node's first word is the bullet, and the node's +4 is the next
// node.
//
// A bullet keeps three floats of position (+0xBC0) and three floats of velocity (+0xBCC). The
// game's own update (0x416700) moves the position by the velocity once per frame, scaled by the
// slowdown factor it applies to everything while the screen is full (0x4D8F58), and it moves a
// bullet that is waiting for its sub-pattern to start by half of that (the state at +0xC0E: 1 =
// in play, 2/3/5 = starting or finishing; 0x200 on the flags at +0x20 holds a bullet, which the
// game neither moves nor draws).
//
// That is exactly what th15 does, so the projection is the same: every bullet in play is moved by
// its velocity times the fraction of a frame the extra presentation stands for, the frame is
// drawn, and the positions are put back right after so the game's own logic never sees them. The
// drawn sprites follow on their own - the bullet draw pass copies the position into the animation
// objects the bullet carries before handing them to the sprite queue (0x4172A4 and the three
// other copies just like it).

static void** const th14_bullet_manager = (void**)0x004DB530;
static const size_t th14_bullet_list_head = 0x80; // in it: the first node of the bullets in play
static const size_t th14_bullet_list_next = 0x04; // in a node: the next node
static const size_t th14_bullet_position = 0xBC0; // in a bullet: its position, 3 floats
static const size_t th14_bullet_velocity = 0xBCC; // in a bullet: what it moves by, 3 floats
static const size_t th14_bullet_state = 0xC0E;    // in a bullet: what it's doing, 16 bits
static const size_t th14_bullet_flags = 0x20;     // in a bullet: 0x200 holds it (and hides it)
// The factor the game scales every movement with (1 unless the screen is full enough for it to
// slow down)
static float* const th14_time_scale = (float*)0x004D8F58;
// How many of the game's updates it runs per rendered frame, minus one
static unsigned char* const th14_frames_per_render = (unsigned char*)0x004D9154;
// The object of the pause screen while it is up, and the flags of it the bullet tasks look at
static void** const th14_pause_object = (void**)0x004DB558;
static const size_t th14_pause_flags = 0x80;

// The bullets that were moved for the presentation being drawn, and the positions they had. The
// pool holds 2001 of them, so the list of the ones in play can't be longer; only the bullets that
// were saved are moved, so a list that somehow grew past the array would only cost the projection
// of the bullets at its end.
static const size_t th14_bullets_saved_max = 2001;
struct Th14SavedBullet {
	void* bullet;
	float position[3];
};
static Th14SavedBullet th14_saved_bullets[th14_bullets_saved_max];
static size_t th14_saved_bullets_count;

// What the last presentation projected, for the diagnostics (see th14_diag_dump)
static unsigned int th14_bullets_in_play;
static unsigned int th14_bullets_moved;

// Whether the pause screen has the picture held. A picture the game isn't updating (or isn't
// drawing) must not be moved forward either.
static bool th14_bullets_held() {
	void* pause = *th14_pause_object;
	if (pause == nullptr)
		return false;
	const unsigned int flags = *(unsigned int*)((char*)pause + th14_pause_flags);
	// What the game's two bullet tasks stop for: the update task for the low bits, the draw task
	// for bit 2 (0x417610 and 0x417640)
	return ((flags | (flags >> 2)) & 1) != 0 || (flags & 4) != 0;
}

// How much of a frame's movement a bullet in this state gets. A bullet that is in play moves a
// whole step, one that is still waiting for its sub-pattern to start moves half of one (0x416700),
// and one that is in neither state doesn't move at all. 0x200 on a bullet holds it - the game
// neither moves nor draws such a bullet.
static float th14_bullet_step_factor(unsigned short state, unsigned int flags) {
	switch (state) {
	case 1:
		return (flags & 0x200) != 0 ? 0.0f : 1.0f;
	case 2:
	case 3:
	case 5:
		return 0.5f;
	default:
		return 0.0f;
	}
}

// Moves every bullet in play forward by t frames (t = 0 is the state the game is in) and remembers
// what it changed, so th14_bullets_undo can put it back.
static void th14_bullets_advance(float t) {
	th14_saved_bullets_count = 0;
	th14_bullets_in_play = 0;
	th14_bullets_moved = 0;

	void* manager = *th14_bullet_manager;
	if (manager == nullptr || th14_bullets_held())
		return;

	unsigned int frames_per_render = *th14_frames_per_render;
	if (frames_per_render > 2) // the game's own setting only goes to 2
		frames_per_render = 2;

	float scale = *th14_time_scale;
	if (!(scale > 0.0f) || scale > 2.0f)
		scale = 1.0f;

	const float step = t * (float)(frames_per_render + 1) * scale;

	void* node = *(void**)((char*)manager + th14_bullet_list_head);
	// The list is at most as long as the pool, and this runs with the game paused in its own frame
	// function, so it can't change under us; the bound is there to keep a list that got corrupted
	// some other way from running away with the frame.
	for (size_t guard = 0; node != nullptr && guard < th14_bullets_saved_max * 4; ++guard) {
		void* bullet = *(void**)node;
		void* next = *(void**)((char*)node + th14_bullet_list_next);
		if (bullet == nullptr)
			break;

		++th14_bullets_in_play;
		const float factor = th14_bullet_step_factor(
			*(unsigned short*)((char*)bullet + th14_bullet_state),
			*(unsigned int*)((char*)bullet + th14_bullet_flags));
		if (factor != 0.0f && th14_saved_bullets_count < th14_bullets_saved_max) {
			float* position = (float*)((char*)bullet + th14_bullet_position);
			const float* velocity = (const float*)((char*)bullet + th14_bullet_velocity);
			Th14SavedBullet& saved = th14_saved_bullets[th14_saved_bullets_count++];
			saved.bullet = bullet;

			const float bullet_step = step * factor;
			for (int axis = 0; axis < 3; ++axis) {
				saved.position[axis] = position[axis];
				position[axis] += velocity[axis] * bullet_step;
			}
			++th14_bullets_moved;
		}

		node = next;
	}
}

// Puts the positions of every bullet th14_bullets_advance moved back, so the game's own logic sees
// the state it left behind and not the projected one.
static void th14_bullets_undo() {
	for (size_t i = 0; i < th14_saved_bullets_count; ++i) {
		const Th14SavedBullet& saved = th14_saved_bullets[i];
		float* position = (float*)((char*)saved.bullet + th14_bullet_position);
		for (int axis = 0; axis < 3; ++axis)
			position[axis] = saved.position[axis];
	}
	th14_saved_bullets_count = 0;
}

// ---------------------------------------------------------------------------
// Frame interpolation, stage 1: the player, its options and its shots
// ---------------------------------------------------------------------------
//
// The player of the stage that is running is at 0x4DB67C (null in the menus and before a stage
// starts). Its animation object sits at +0x14 and the render pass of the player copies its
// position (+0x5E0) into that object's position before handing it to the sprite queue (0x44EC70),
// so moving the position moves the player's sprite the same way the bullets' position does.
//
// Everything else the player puts on screen is positioned by the update instead, so those sprites
// have to be moved themselves:
//
//   - the options (the sub-shots that sit beside the player) are 8 slots of 0xE4 bytes at +0xD6EC.
//     A slot that is up keeps two sprite object ids at +0xB0/+0xB4 and a position in 1/128 pixel
//     units at +0x5C, which the update walks toward the player's position (0x44D330).
//   - the player's shots (the bullets it fires) are 256 slots of 0xD0 bytes at +0x6C8. A live slot
//     keeps its sprite object id at +0x14 and its position in pixels at +0x54, which the update
//     copies into the sprite (0x4510B0).
//
// The player moves by what the input says rather than by a velocity that could be read back, and
// the options and shots move by what comes out of that plus their own motion, so what each sprite
// moves by in a frame is measured instead of read: every game frame the positions are compared
// with the ones of the frame before, and the extra presentations move the sprites by that
// difference times the fraction of a frame they stand for. Something that didn't move - a held
// picture, a paused game - comes out as zero on its own.

static void** const th14_player = (void**)0x004DB67C;
static const size_t th14_player_position = 0x5E0;        // the player's own position, 3 floats
static const size_t th14_player_options = 0xD6EC;        // 8 slots of 0xE4 bytes
static const size_t th14_player_option_count = 8;
static const size_t th14_player_option_size = 0xE4;
static const size_t th14_player_option_live = 0x00;      // nonzero while the slot is up
static const size_t th14_player_option_position = 0x5C;  // 2 ints, in 1/128 of a pixel
static const size_t th14_player_option_sprite = 0xB0;    // 2 object ids
static const size_t th14_player_shots = 0x6C8;           // 256 slots of 0xD0 bytes
static const size_t th14_player_shot_count = 256;
static const size_t th14_player_shot_size = 0xD0;
static const size_t th14_player_shot_live = 0x98;        // nonzero while the shot is in use
static const size_t th14_player_shot_sprite = 0x14;      // the sprite's object id
static const size_t th14_player_shot_position = 0x54;    // its position, 3 floats in pixels
static const size_t th14_object_position = 0x59C;        // in an animation object
static const float th14_subpixel = 1.0f / 128.0f;
// Nothing on screen moves this far in one frame (the player's own speed is a few pixels), so a
// difference bigger than this is a slot that was handed to another object, not a movement
static const float th14_max_step = 48.0f;

// One thing to move for an extra presentation: the animation object whose position is shifted, and
// where the movement that drives it is measured (the object itself for the player, the slot of the
// option or shot for their sprites)
static const size_t th14_tracked_max = 1 + th14_player_option_count * 2 + th14_player_shot_count;

// What one sprite moved by. This has to outlive the frame it was measured in: what is on screen
// changes from frame to frame (shots come and go), so the list of the sprites to move is built
// again every frame, and the position of the frame before can't be kept in that list.
struct Th14SpriteMotion {
	void* sprite;         // the animation object it is about, null while the entry is free
	unsigned int frame;   // the frame it was last measured in
	float last_x, last_y;
	float delta_x, delta_y;
};

struct Th14Tracked {
	float* shift;             // the position that gets moved (3 floats)
	Th14SpriteMotion* motion; // what it moved by, from the frame before
};

static Th14SpriteMotion th14_motions[th14_tracked_max];
static unsigned int th14_motion_frame = 1;
static Th14Tracked th14_tracked[th14_tracked_max];
static size_t th14_tracked_count;
static float th14_tracked_saved[th14_tracked_max][3];

// Diagnostics: the largest movement the player's own position had since the last dump
static float th14_player_movement;
// Diagnostics: how many sprites this frame couldn't be given a movement record. It stays 0 unless
// every record in the table belongs to a sprite that is on screen right now (see
// th14_sprite_motion) - which is what the sprites quietly stopping to move looked like before the
// records were recycled.
static unsigned int th14_sprites_untracked;

// The animation object a sprite id refers to: the same lookup the game does (0x47F0A0), written
// out instead of called. Calling it would be the way to get this wrong - it takes its argument on
// the stack as a thiscall-style member, and a call through a function pointer is not put together
// with that in mind, so the garbage it gets back hands back a pointer that is not an animation
// object at all. The manager at 0x4F56CC keeps its objects in slots of 0x5E4 starting at +0xDC,
// each of them with the id it belongs to at +0x540 and an "in use" byte at +0x5DC, and ids whose
// low 13 bits are 0x1FFF are on one of the manager's two lists instead (the player's sprites never
// are).
static void* th14_object_by_id(unsigned int id) {
	const unsigned int index = id & 0x1FFF;
	if (id == 0 || index == 0x1FFF)
		return nullptr;

	void* manager = *(void**)0x004F56CC;
	if (manager == nullptr)
		return nullptr;

	char* object = (char*)manager + 0xDC + (size_t)index * 0x5E4;
	if (*(unsigned char*)(object + 0x5DC) == 0)
		return nullptr;
	if (*(unsigned int*)(object + 0x540) != id)
		return nullptr;
	return object;
}

// The record of a sprite, made the first time it is seen (sprite is the position it is moved by).
//
// Nothing tells a sprite that is gone from a slot that is still up, so a record can't be dropped
// when its sprite stops being drawn - which means they have to be recycled instead. Without that,
// everything a stage ever puts on screen keeps a record of its own: the shots alone go through
// hundreds of them, the table fills up, and from then on every sprite that isn't in it already
// quietly stops being moved. That is what a stage restart runs into - the player is built again at
// another address, asks for a record, finds the table full, and its projection stops (it works
// again whenever the new player happens to land where the old one was, which is what makes it look
// like it only happens sometimes).
//
// The one that was measured longest ago is the one recycled: a record that wasn't measured in the
// frame right before this one has nothing to compare against anyway. A record that is in use this
// frame is never taken, so the sprite it belongs to keeps moving with the others.
static Th14SpriteMotion* th14_sprite_motion(void* sprite) {
	Th14SpriteMotion* empty = nullptr;
	Th14SpriteMotion* oldest = nullptr;
	for (size_t i = 0; i < th14_tracked_max; ++i) {
		Th14SpriteMotion& motion = th14_motions[i];
		if (motion.sprite == sprite)
			return &motion;
		if (motion.sprite == nullptr) {
			if (empty == nullptr)
				empty = &motion;
			continue;
		}
		if (motion.frame == th14_motion_frame)
			continue;
		if (oldest == nullptr || motion.frame < oldest->frame)
			oldest = &motion;
	}

	Th14SpriteMotion* motion = empty != nullptr ? empty : oldest;
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
// out of the position that the game's own update left in watch
static void th14_track(float* shift, const void* watch, bool watch_is_int) {
	if (th14_tracked_count >= th14_tracked_max)
		return;
	// Two ids can name the same sprite (the game doesn't mind), and moving one twice would put it
	// twice as far out and only be able to put back one of the two
	for (size_t i = 0; i < th14_tracked_count; ++i) {
		if (th14_tracked[i].shift == shift)
			return;
	}
	Th14SpriteMotion* motion = th14_sprite_motion(shift);
	if (motion == nullptr) {
		++th14_sprites_untracked;
		return;
	}

	float x, y;
	if (watch_is_int) {
		x = (float)*(const int*)watch * th14_subpixel;
		y = (float)*((const int*)watch + 1) * th14_subpixel;
	} else {
		x = *(const float*)watch;
		y = *((const float*)watch + 1);
	}

	// Only a sprite that was measured in the frame right before this one has something to compare
	// against (one that just appeared, or that came back after the game didn't draw for a while,
	// hasn't moved as far as this can tell)
	if (motion->frame + 1 == th14_motion_frame) {
		const float dx = x - motion->last_x;
		const float dy = y - motion->last_y;
		// The pools hand their slots out again, and the object that used to be there is somewhere
		// else entirely - that difference is not something to project
		if (dx > -th14_max_step && dx < th14_max_step && dy > -th14_max_step && dy < th14_max_step) {
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
	motion->frame = th14_motion_frame;
	motion->last_x = x;
	motion->last_y = y;

	Th14Tracked& tracked = th14_tracked[th14_tracked_count++];
	tracked.shift = shift;
	tracked.motion = motion;
}

// Collects what is on screen this frame and works out what it moved by since the frame before.
// This runs once per game frame, from the game's own present (see th14_present_hook).
static void th14_player_measure() {
	th14_tracked_count = 0;
	th14_sprites_untracked = 0;
	++th14_motion_frame;

	void* player = *th14_player;
	if (player == nullptr)
		return;

	// The player itself: its own position is what the render pass reads
	th14_track((float*)((char*)player + th14_player_position),
		(const char*)player + th14_player_position, false);
	if (th14_tracked_count > 0) {
		const Th14SpriteMotion* motion = th14_tracked[0].motion;
		const float x = motion->delta_x < 0.0f ? -motion->delta_x : motion->delta_x;
		const float y = motion->delta_y < 0.0f ? -motion->delta_y : motion->delta_y;
		const float magnitude = x > y ? x : y;
		if (magnitude > th14_player_movement)
			th14_player_movement = magnitude;
	}

	// The options, and the shots, are only measured while they are up
	for (size_t i = 0; i < th14_player_option_count; ++i) {
		const char* slot = (const char*)player + th14_player_options + i * th14_player_option_size;
		if (*(const int*)(slot + th14_player_option_live) == 0)
			continue;
		for (int sprite = 0; sprite < 2; ++sprite) {
			void* object = th14_object_by_id(*(const unsigned int*)(slot + th14_player_option_sprite + sprite * 4));
			if (object == nullptr)
				continue;
			th14_track((float*)((char*)object + th14_object_position),
				slot + th14_player_option_position, true);
		}
	}

	for (size_t i = 0; i < th14_player_shot_count; ++i) {
		const char* slot = (const char*)player + th14_player_shots + i * th14_player_shot_size;
		if (*(const int*)(slot + th14_player_shot_live) == 0)
			continue;
		void* object = th14_object_by_id(*(const unsigned int*)(slot + th14_player_shot_sprite));
		if (object == nullptr)
			continue;
		th14_track((float*)((char*)object + th14_object_position),
			slot + th14_player_shot_position, false);
	}
}

// Moves the player, its options and its shots t frames ahead of the state the game is in, and
// remembers what it changed
static void th14_player_advance(float t) {
	for (size_t i = 0; i < th14_tracked_count; ++i) {
		const Th14Tracked& tracked = th14_tracked[i];
		float* position = tracked.shift;
		for (int axis = 0; axis < 3; ++axis) {
			th14_tracked_saved[i][axis] = position[axis];
		}
		position[0] += tracked.motion->delta_x * t;
		position[1] += tracked.motion->delta_y * t;
	}
}

static void th14_player_undo() {
	for (size_t i = 0; i < th14_tracked_count; ++i) {
		float* position = th14_tracked[i].shift;
		for (int axis = 0; axis < 3; ++axis)
			position[axis] = th14_tracked_saved[i][axis];
	}
}

// ---------------------------------------------------------------------------
// Frame interpolation, stage 1: the straight lasers
// ---------------------------------------------------------------------------
//
// The lasers of a stage live in a manager of their own (0x4DB664, 0x600 bytes). It is a task: its
// update (0x43A570) and its draw (0x43A710) both walk the same list, whose first laser is at
// manager+0x18 and whose links are the object's own +0x08 (next) and +0x04 (previous). Each walk
// calls the object's own virtual function for it - the update one at vtable+0x10, the draw one at
// vtable+0x14 - and the draw one is what matters here.
//
// A line laser's draw (0x43C3C0) builds its picture out of three of its own fields: the position
// at +0x54, the angle at +0x6C and the length at +0x70. It copies the position into the sprites it
// draws and puts the far end of the beam at position + polar(angle, length), so moving those
// fields moves the laser, exactly like the player's position moves the player. The infinite laser
// (0x43E420) reads the same position and angle.
//
// The same measurement is used as for the player, for the same reason (a laser's own update is
// what moves it, and it grows and turns as it does), and the curve laser is left out on purpose:
// LaserCurveInf (vtable 0x4BE2FC) is the one the requirements say not to do. The others -
// LaserLineInf (0x4BE434), LaserInfiniteInf (0x4BE3CC) and LaserBeamInf (0x4BE364) - are straight
// and are handled, and anything else that turns up in the list (the manager's own root entry, for
// one) is skipped by only ever looking at objects with one of those three vtables.

static void** const th14_laser_manager = (void**)0x004DB664;
static const size_t th14_laser_list = 0x18;      // the first laser of the list
static const size_t th14_laser_next = 0x08;      // in a laser: the next one
static const size_t th14_laser_position = 0x54;  // where it starts, 3 floats
static const size_t th14_laser_angle = 0x6C;     // the direction it points in, radians
static const size_t th14_laser_length = 0x70;    // how far it reaches

static const void* const th14_straight_lasers[] = {
	(void*)0x004BE434, // LaserLineInf
	(void*)0x004BE3CC, // LaserInfiniteInf
	(void*)0x004BE364, // LaserBeamInf
};

static const size_t th14_lasers_max = 0x200 + 1; // the manager's own limit, plus its root entry

// What one laser moved by, kept across frames (the list is walked again every frame, and a laser
// that is gone must not take its record with it - a slot that comes back later is another laser)
struct Th14LaserMotion {
	void* laser;
	unsigned int frame;
	float last_x, last_y, last_angle, last_length;
	float delta_x, delta_y, delta_angle, delta_length;
};

struct Th14LaserMoved {
	void* laser;
	float position[3];
	float angle, length;
};

static Th14LaserMotion th14_laser_motions[th14_lasers_max];
static Th14LaserMoved th14_lasers_moved[th14_lasers_max];
static size_t th14_lasers_moved_count;
static unsigned int th14_laser_frame = 1;
static unsigned int th14_lasers_seen;

// Whether this is one of the straight lasers (the curve one is deliberately not one of them)
static bool th14_is_straight_laser(const void* laser) {
	const void* vtable = *(const void* const*)laser;
	for (size_t i = 0; i < sizeof(th14_straight_lasers) / sizeof(th14_straight_lasers[0]); ++i) {
		if (vtable == th14_straight_lasers[i])
			return true;
	}
	return false;
}

// The lasers are handed out and freed one at a time, so their records have to be recycled the same
// way the sprites' are (see th14_sprite_motion)
static Th14LaserMotion* th14_laser_motion(void* laser) {
	for (size_t i = 0; i < th14_lasers_max; ++i) {
		if (th14_laser_motions[i].laser == laser)
			return &th14_laser_motions[i];
	}

	Th14LaserMotion* empty = nullptr;
	Th14LaserMotion* oldest = nullptr;
	for (size_t i = 0; i < th14_lasers_max; ++i) {
		Th14LaserMotion& motion = th14_laser_motions[i];
		if (motion.laser == nullptr) {
			if (empty == nullptr)
				empty = &motion;
			continue;
		}
		if (motion.frame == th14_laser_frame)
			continue;
		if (oldest == nullptr || motion.frame < oldest->frame)
			oldest = &motion;
	}

	Th14LaserMotion* motion = empty != nullptr ? empty : oldest;
	if (motion == nullptr)
		return nullptr;
	motion->laser = laser;
	motion->frame = 0;
	motion->last_x = motion->last_y = 0.0f;
	motion->last_angle = motion->last_length = 0.0f;
	motion->delta_x = motion->delta_y = 0.0f;
	motion->delta_angle = motion->delta_length = 0.0f;
	return motion;
}

// Looks at every straight laser in play and works out what it moved by since the frame before.
// This runs once per game frame, from the game's own present (see th14_present_hook).
static void th14_lasers_measure() {
	++th14_laser_frame;
	th14_lasers_seen = 0;

	void* manager = *th14_laser_manager;
	if (manager == nullptr)
		return;

	void* laser = *(void**)((char*)manager + th14_laser_list);
	// The list is the game's own and this runs where nothing can change it; the bound is only so a
	// list that got broken some other way can't run away with the frame
	for (size_t guard = 0; laser != nullptr && guard < th14_lasers_max * 4; ++guard) {
		void* next = *(void**)((char*)laser + th14_laser_next);
		if (th14_is_straight_laser(laser)) {
			++th14_lasers_seen;
			Th14LaserMotion* motion = th14_laser_motion(laser);
			if (motion != nullptr) {
				const float x = *(const float*)((char*)laser + th14_laser_position);
				const float y = *(const float*)((char*)laser + th14_laser_position + 4);
				const float angle = *(const float*)((char*)laser + th14_laser_angle);
				const float length = *(const float*)((char*)laser + th14_laser_length);

				// Only a laser that was measured in the frame right before this one has something
				// to compare against - one that just appeared has not moved as far as this can tell
				if (motion->frame + 1 == th14_laser_frame) {
					float dx = x - motion->last_x;
					float dy = y - motion->last_y;
					float dangle = angle - motion->last_angle;
					float dlength = length - motion->last_length;

					// The angle is an angle: a difference of nearly a full turn is the same
					// angle, and neither that nor a jump across the screen is a movement
					while (dangle > 3.14159265f)
						dangle -= 6.28318531f;
					while (dangle < -3.14159265f)
						dangle += 6.28318531f;
					if (dx <= -th14_max_step || dx >= th14_max_step ||
						dy <= -th14_max_step || dy >= th14_max_step ||
						dangle <= -1.0f || dangle >= 1.0f || dlength <= -256.0f || dlength >= 256.0f) {
						dx = dy = dangle = dlength = 0.0f;
					}

					motion->delta_x = dx;
					motion->delta_y = dy;
					motion->delta_angle = dangle;
					motion->delta_length = dlength;
				} else {
					motion->delta_x = motion->delta_y = 0.0f;
					motion->delta_angle = motion->delta_length = 0.0f;
				}
				motion->frame = th14_laser_frame;
				motion->last_x = x;
				motion->last_y = y;
				motion->last_angle = angle;
				motion->last_length = length;
			}
		}
		laser = next;
	}
}

// Moves the straight lasers t frames ahead of the state the game is in, and remembers what it
// changed
static void th14_lasers_advance(float t) {
	th14_lasers_moved_count = 0;

	for (size_t i = 0; i < th14_lasers_max; ++i) {
		const Th14LaserMotion& motion = th14_laser_motions[i];
		if (motion.laser == nullptr || motion.frame != th14_laser_frame)
			continue;
		if (th14_lasers_moved_count >= th14_lasers_max)
			break;

		void* laser = motion.laser;
		Th14LaserMoved& moved = th14_lasers_moved[th14_lasers_moved_count++];
		moved.laser = laser;

		float* position = (float*)((char*)laser + th14_laser_position);
		float* angle = (float*)((char*)laser + th14_laser_angle);
		float* length = (float*)((char*)laser + th14_laser_length);
		for (int axis = 0; axis < 3; ++axis)
			moved.position[axis] = position[axis];
		moved.angle = *angle;
		moved.length = *length;

		position[0] += motion.delta_x * t;
		position[1] += motion.delta_y * t;
		*angle += motion.delta_angle * t;
		*length += motion.delta_length * t;
	}
}

static void th14_lasers_undo() {
	for (size_t i = 0; i < th14_lasers_moved_count; ++i) {
		const Th14LaserMoved& moved = th14_lasers_moved[i];
		float* position = (float*)((char*)moved.laser + th14_laser_position);
		for (int axis = 0; axis < 3; ++axis)
			position[axis] = moved.position[axis];
		*(float*)((char*)moved.laser + th14_laser_angle) = moved.angle;
		*(float*)((char*)moved.laser + th14_laser_length) = moved.length;
	}
	th14_lasers_moved_count = 0;
}

// The plain calls the render block makes into the game
static auto th14_reset_sprite_queue = (void(*)())0x00475E60;
static auto th14_prepare_render = (void(__fastcall*)(void*))0x00445940; // ECX = the render context
static auto th14_render_objects = (void(*)())0x004013B0;                // renders every object
static auto th14_draw_sprite_queue = (void(__fastcall*)(void*))0x00475EB0; // ECX = the queue's owner

// Where the user's tool builds its frame and where it draws it. thprac's th14 module starts its
// frame (its NewFrame, its UI code, and its EndFrame) from a hook on the return of the scene update
// at 0x40138A, and draws the overlay it built from a hook on the return of the object render at
// 0x40149A. The extra presentations render the scene again, which erases the overlay the game's own
// pass drew, so the tool has to draw it again for every one of them (see tool_state.h - its frame
// state is found at runtime and set again there, which draws the overlay without running any of the
// tool's own code).
static unsigned char* const th14_tool_frame_address = (unsigned char*)0x0040138A;
static unsigned char* const th14_tool_draw_address = (unsigned char*)0x0040149A;

// The viewport the scene is drawn with, taken from the frame the game's own render ran in: the
// extra presentations have to draw with the same one, and it can differ from what's left over in
// the device (the pause screens, for example, render into a smaller part of the back buffer)
static D3DVIEWPORT9 th14_scene_viewport = {};

// Remembers the viewport the scene is about to be drawn with, then lets the game's own render run
// exactly as it always did (see th14_extra_presentation for what it's for)
static void th14_render_hook() {
	IDirect3DDevice9* device = d3d9_hooked_device();
	if (device != nullptr)
		device->GetViewport(&th14_scene_viewport);

	// The tool's frame is built by now (it runs from the scene update's return, before this) but
	// not drawn yet (that happens on the return of the render below): the point to watch it from
	ToolState::WatchFrameReady(th14_tool_frame_address, th14_tool_draw_address);

	th14_render_objects();
}

// The number of presentations per second the config asks for. 0 means interpolation is off.
//
// "-1" picks the largest multiple of the game's frame rate that still fits the display refresh
// (a 190hz display and a 60fps game give 3), a positive value is that multiple, and the
// "*<frame rate>" form is that frame rate no matter what the game's own is.
static double th14_present_rate() {
	if (Config::InterpolationFPS)
		return (double)Config::InterpolationFPS;

	int multiplier = Config::Interpolation;
	if (multiplier < 0) {
		UINT display = Limiter::DisplayRefresh();
		UINT logic = (UINT)(Limiter::game_fps + 0.5);
		if (display == 0 || logic == 0)
			return 0.0;
		multiplier = (int)(display / logic);
	}
	if (multiplier < 2)
		return 0.0;
	return Limiter::game_fps * (double)multiplier;
}

// Diagnostics: press U to write the presentation timing of the last frames to oilp_diag.txt
static const unsigned int th14_diag_frames_max = 300;
static const unsigned int th14_diag_presents_max = 8;
struct Th14DiagFrame {
	LARGE_INTEGER base;                                      // the game's own present of the frame
	unsigned int presents;
	LARGE_INTEGER present_time[th14_diag_presents_max];      // when each presentation happened
};
static Th14DiagFrame th14_diag_frames[th14_diag_frames_max];
static unsigned int th14_diag_count;
static bool th14_diag_key_down;

static LARGE_INTEGER th14_perf_freq;

static void th14_diag_dump() {
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

	unsigned int count = th14_diag_count < th14_diag_frames_max ? th14_diag_count : th14_diag_frames_max;
	unsigned int start = th14_diag_count < th14_diag_frames_max ? 0 : th14_diag_count % th14_diag_frames_max;

	fprintf(file, "oilp frame interpolation diagnostics\n");
	fprintf(file, "logic frame rate: %.2f, configured presentation rate: %.2f\n",
		Limiter::game_fps, th14_present_rate());
	fprintf(file, "frames recorded: %u\n", count);
	fprintf(file, "bullets: %u in play, %u projected forward (last game frame)\n",
		th14_bullets_in_play, th14_bullets_moved);
	fprintf(file, "player: %u sprite(s) moved (the player, its options and its shots)\n",
		(unsigned int)th14_tracked_count);
	fprintf(file, "player: %u sprite(s) this frame had no movement record (should stay 0)\n",
		th14_sprites_untracked);
	fprintf(file, "player movement: up to %.2f px per frame since the last dump\n", th14_player_movement);
	th14_player_movement = 0.0f;
	fprintf(file, "lasers: %u straight laser(s) in play (the curve ones are left alone)\n",
		th14_lasers_seen);

	double presents = 0.0;
	double first = 0.0;
	double last = 0.0;
	for (unsigned int i = 0; i < count; ++i) {
		const Th14DiagFrame& frame = th14_diag_frames[(start + i) % th14_diag_frames_max];
		fprintf(file, "frame %4u: %u present(s)", i, frame.presents);
		for (unsigned int p = 0; p < frame.presents && p < th14_diag_presents_max; ++p)
			fprintf(file, " [%u] %.2fms", p,
				(double)(frame.present_time[p].QuadPart - frame.base.QuadPart) * 1000.0 / (double)th14_perf_freq.QuadPart);
		fprintf(file, "\n");
		presents += frame.presents;
		if (i == 0)
			first = (double)frame.base.QuadPart;
		last = (double)frame.base.QuadPart;
	}
	if (last > first)
		fprintf(file, "average: %.3f presents per frame, %.1f presentations per second\n",
			presents / (double)count, presents * (double)th14_perf_freq.QuadPart / (last - first));

	fclose(file);
	printf("Wrote oilp_diag.txt\n");
}

// One extra presentation of the frame: the same render block the game runs, once more, so the state
// the game is in is drawn and presented again, with the objects projected t frames ahead of it -
// t = 0 is the state the game's own presentation showed, and the presentations of one frame reach
// t = 1, which is where the game's next one takes over. The tool's frame is readied again, so the
// object render below draws its overlay into this presentation the same way it does in the game's
// own one - and it reads the projected positions, so a tool that draws something on the objects
// (thprac's hitbox display, for example) stays on them.
static void th14_extra_presentation(float t) {
	IDirect3DDevice9* device = d3d9_hooked_device();
	if (device == nullptr)
		return;

	device->BeginScene();
	// The game's own pass set the viewport before this point, and the pause screens and the menus
	// change it, so put back whatever the scene was drawn with (see th14_render_hook)
	if (th14_scene_viewport.Width != 0)
		device->SetViewport(&th14_scene_viewport);
	th14_reset_sprite_queue();
	*(DWORD*)0x004D9960 = 0xFF;
	th14_prepare_render((void*)0x004D8F60);
	ToolState::ReadyToDraw();
	// The objects are drawn where they will be t frames from now; the render bakes the positions
	// into the sprite queue, so they can go back right after it
	th14_bullets_advance(t);
	th14_player_advance(t);
	th14_lasers_advance(t);
	th14_render_objects();
	th14_lasers_undo();
	th14_player_undo();
	th14_bullets_undo();
	th14_draw_sprite_queue(*(void**)0x004F56CC);
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
static HRESULT th14_present_hook() {
	IDirect3DDevice9* device = d3d9_hooked_device();
	if (device == nullptr)
		return E_FAIL;

	static bool freq_queried = false;
	if (!freq_queried) {
		freq_queried = true;
		QueryPerformanceFrequency(&th14_perf_freq);
	}

	LARGE_INTEGER base;
	QueryPerformanceCounter(&base);

	// The game's own update has run and it is about to put that frame on screen: this is the one
	// point per frame where the movement of the player, its options and its shots can be measured
	th14_player_measure();
	th14_lasers_measure();

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
	LARGE_INTEGER present_time[th14_diag_presents_max];
	present_time[0] = base;

	double rate = th14_present_rate();
	// Nothing more to present if the presentation that just happened failed (a lost device, for
	// example) - the game's own code right after this handles that
	if (SUCCEEDED(result) && rate > 0.0 && Limiter::game_fps > 0.0) {
		static bool rate_printed = false;
		if (!rate_printed) {
			rate_printed = true;
			printf("Frame interpolation: %.1f presentations per second (%.2f per game frame)\n",
				rate, rate / Limiter::game_fps);
		}

		// How many presentations this frame gets. The game's own present is one of them, and the
		// fraction is carried over to the next frame, so a rate that isn't a multiple of the game's
		// frame rate still averages out ("*144" with 60fps logic presents 2 and 3 alternately).
		static double presentations_owed = 0.0;
		presentations_owed += rate / Limiter::game_fps - 1.0;
		unsigned int extras = 0;
		while (presentations_owed >= 1.0 - 1e-9 && extras < 32) {
			presentations_owed -= 1.0;
			++extras;
		}

		LARGE_INTEGER frame_wait = Limiter::FrameWait();
		if (extras > 0 && frame_wait.QuadPart > 0) {
			// The game's own present is the first presentation of the frame; the extras go after
			// it, one every this many performance counter ticks, evenly spread over the rest of
			// the frame. Keeping the presentations evenly spaced matters: each one draws the state
			// a little further along, so an uneven spacing is an uneven looking motion.
			double spacing = (double)frame_wait.QuadPart / (double)(extras + 1);
			if (spacing >= 1.0) {
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
					target.QuadPart = base.QuadPart + (LONGLONG)(spacing * (double)k + 0.5);
					if (has_deadline && target.QuadPart + frame_work.QuadPart > deadline.QuadPart)
						continue;

					Limiter::WaitUntil(target);

					if (presents < th14_diag_presents_max)
						QueryPerformanceCounter(&present_time[presents]);
					++presents;

					// Where this presentation sits between the game's own one and the next one:
					// the frames of one game frame are spread evenly over it, so the k-th of
					// extras + 1 of them draws the state k/(extras + 1) frames along (stage 1)
					th14_extra_presentation((float)k / (float)(extras + 1));
				}
			}
		}
	}

	// Record the frame for the diagnostics
	{
		Th14DiagFrame& frame = th14_diag_frames[th14_diag_count % th14_diag_frames_max];
		frame.base = base;
		frame.presents = presents;
		for (unsigned int p = 0; p < th14_diag_presents_max; ++p)
			frame.present_time[p] = p < presents ? present_time[p] : base;
		++th14_diag_count;
	}

	// U dumps the diagnostics
	if (GetAsyncKeyState('U') & 0x8000) {
		if (!th14_diag_key_down) {
			th14_diag_key_down = true;
			th14_diag_dump();
		}
	} else {
		th14_diag_key_down = false;
	}

	return result;
}

// The present the game sets up at each present call site: the arguments are pushed and the device
// is called, 14 bytes in all (see th14_install_patches)
static const size_t th14_present_setup_size = 14;

// Jumps over the present the game sets up after its present call site, so the code after it runs
// with the result the hook left in eax
static void th14_skip_present_setup(DWORD address) {
	BYTE patch[th14_present_setup_size];
	patch[0] = 0xE9;
	*(DWORD*)(patch + 1) = (address + th14_present_setup_size) - (address + 5);
	for (size_t i = 5; i < th14_present_setup_size; ++i)
		patch[i] = 0x90;
	patch_bytes(address, patch, sizeof(patch));
}

void th14_install_patches() {
	{
		// Skip the original frame limiter
		BYTE patch[] = { 0xEB, 0x4A };
		patch_bytes(0x0046A76E, patch, sizeof(patch));
	}
	{
		// Force fast input latency mode
		BYTE patch[] = { 0xEB };
		patch_bytes(0x00469A20, patch, sizeof(patch)); // Skip over automatic
		patch_bytes(0x00469A35, patch, sizeof(patch)); // Skip over normal
	}
	{
		// Hook window update
		patch_call(0x00469A51, th14_window_update_hook);
	}
	if (Config::D3D9Ex) {
		// Redirect Direct3DCreate9 call
		// IAT is being hooked, but thcrap also hooks Direct3DCreate9 in the same place
		// This should force our Direct3DCreate9 hook to be loaded no matter what
		patch_call(0x0046952C, Direct3DCreate9_hook);

		// Extra NOP is needed because we're replacing a FF 15 call, which is 6 bytes long
		BYTE patch[] = { 0x90 };
		patch_bytes(0x00469531, patch, sizeof(patch));
	}
	if (Config::ReplaySpeedControl) {
		// Skip the original replay speed control stuff
		BYTE patch[] = { 0xEB, 0x1D };
		patch_bytes(0x00455E82, patch, sizeof(patch));
	}
	if (Config::Interpolation || Config::InterpolationFPS) {
		// Take over the object render, only to remember the viewport the scene is drawn with (see
		// th14_render_hook); the render itself runs exactly the way it always did
		static const DWORD render_calls[] = { 0x0046A45D, 0x0046A86D, 0x0046AA0C };
		unsigned int render_patched = 0;
		for (DWORD address : render_calls) {
			BYTE* render_call = (BYTE*)address;
			if (render_call[0] == 0xE8) {
				patch_call(address, (void*)th14_render_hook);
				++render_patched;
			} else {
				printf("th14 render call at 0x%x doesn't look like a known one, skipping!\n", address);
			}
		}

		// Take over the presents in the three frame functions. The instruction that loads the
		// device for the present is 5 bytes, which is exactly what the hook's call needs, and the
		// 14 bytes after it (the arguments and the present call itself) are skipped over - the hook
		// presents with the same device and arguments the game would have used.
		static const DWORD present_sites[] = { 0x0046A67A, 0x0046A8D6, 0x0046AAEC };
		unsigned int present_patched = 0;
		for (DWORD address : present_sites) {
			BYTE* present_site = (BYTE*)address;
			if (present_site[0] == 0xA1 && *(DWORD*)(present_site + 1) == 0x004D8F68) {
				patch_call(address, (void*)th14_present_hook);
				th14_skip_present_setup(address + 5);
				++present_patched;
			} else {
				printf("th14 present call at 0x%x doesn't look like a known one, skipping!\n", address);
			}
		}
		printf("Frame interpolation: %u render call(s) and %u present call(s) taken over (setting %d)\n",
			render_patched, present_patched,
			Config::InterpolationFPS ? (int)Config::InterpolationFPS : Config::Interpolation);

		// The overlay shows the frame rate the frames are actually presented at, which only says
		// something (and is only higher than the game's own rate) with the interpolation on - so
		// it is only shown when that and the overlay are both on
		overlay_show_present_rate(Config::ShowOverlay && (Config::Interpolation != 0 || Config::InterpolationFPS != 0));
		// ...and it goes right after the line the patch itself draws in the bottom left corner,
		// rather than at the bottom right corner of the window where it would sit on top of the
		// frame rate and slowdown readout thprac draws there (see overlay.h)
		overlay_set_present_rate_anchor(0, 0);
		overlay_set_present_rate_follow_text(true);
	}
}

FPSTarget th14_replay_callback() {
	if (*CReplayManager::InstancePtr && (*CReplayManager::InstancePtr)->mode == 1) {
		// TODO: Reverse engineer this struct instead of being lazy
		auto input = *(DWORD*)0x004D6878;
		if (input & InputState::Focus)
			return FPSTarget::ReplaySlow;
		else if (input & (InputState::Skip | InputState::Shoot))
			return FPSTarget::ReplaySkip;
	}
	return FPSTarget::Game;
}
