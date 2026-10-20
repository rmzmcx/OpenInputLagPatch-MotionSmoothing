// Touhou 11: Subterranean Animism v1.00a

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
#include "touhou11.h"

using namespace Touhou11;

CReplayManager** CReplayManager::InstancePtr = (CReplayManager**)0x004A8EB8;

int __stdcall th11_window_update_hook(void* self) {
	Limiter::Tick();
	return CWindowManager__UpdateFast(self);
}

// ---------------------------------------------------------------------------
// Frame interpolation, stage 1: project the bullets forward
// ---------------------------------------------------------------------------
//
// The bullets of a stage live in the bullet manager at 0x4A8D68, which FUN_00408390 allocates as
// a single 0x46D678 byte block (the manager the game itself is, since the stage build stores it
// in that global) and FUN_00408450 then fills in: it loads bullet.anm into manager+0x46D674 (the
// four bytes of the block that aren't part of the pool), points manager+0x10 at the pool, which
// starts at manager+0x64 and holds 2001 bullets of 0x910 bytes each, and registers the manager's
// two tasks - the update one (thunk 0x408EC0) and the draw one (thunk 0x408EF0). Every bullet
// carries an animation object of its own at +0x08.
//
// The manager's update (FUN_00408D00) runs the bullets and rebuilds the six in-play lists at
// manager+0x14 - one head and one tail per column, so manager+0x14..0x28 are the heads and
// manager+0x2C..0x40 the tails, exactly the layout th12 uses. It walks the pool (2000 slots,
// FUN_00408D00 from manager+0x64 with the state at +0x4B2 deciding) and, for every bullet that is
// in play, runs it through FUN_00408900 and threads it onto the list of the column the bullet
// keeps as a float at +0x4CC; the bullet's link to the next one of its column is its own +0x4B8,
// and the manager counts the ones in play at +0x5C. The manager's draw (FUN_00408E00 and
// FUN_00408E20, which the draw thunk calls once per column) walks those same lists, which makes
// them exactly the bullets that are drawn - so that is the list walked here, and it is read after
// the game's own update ran, which is the case by the time the frame is presented.
//
// A bullet moves by its velocity: FUN_00408900 adds the three floats at +0x448/+0x44C/+0x450 to
// the position at +0x43C/+0x440/+0x444, scaled by the global 0x4A7948 (1.0 unless the game slows
// everything down because the screen is full enough), once per frame. It moves at half of that
// while its state at +0x4B2 is 2 or 3 (0 is free, 1 is a bullet in flight; the half is the double
// at 0x497E60, which is 0.5), and not at all in any other state. The states are what 0x408Fxx
// switches on inside FUN_00408900, and the two half-speed paths are the same code as the full
// speed one with that one factor more.
//
// The drawn sprites follow on their own: the draw pass copies this position into the sprite fields
// at +0x3E4/+0x3E8/+0x3EC (adding the play field's own origin, the doubles at 0x497EE8, 0x497EA0
// and 0x497EE0) and hands the animation object at +0x08 to the sprite queue (0x451EF0), so moving
// the position is enough - the same rule as in th12 to th15.

static void** const th11_bullet_manager = (void**)0x004A8D68;
static const size_t th11_bullet_lists = 0x14;      // in the manager: the first column's head
static const size_t th11_bullet_columns = 6;       // ...and how many columns there are
static const size_t th11_bullet_next = 0x4B8;      // in a bullet: the next one of its column
static const size_t th11_bullet_position = 0x43C;  // in a bullet: its position, 3 floats
static const size_t th11_bullet_velocity = 0x448;  // in a bullet: what it moves by, 3 floats
static const size_t th11_bullet_state = 0x4B2;     // in a bullet: what it's doing, 16 bits
// The factor the game scales every movement with (1 unless the screen is full enough for it to
// slow down)
static float* const th11_time_scale = (float*)0x004A7948;
// How many of the game's updates it runs per rendered frame, minus one
static unsigned char* const th11_frames_per_render = (unsigned char*)0x004C3466;
// The object of the pause screen while it is up, and the flags of it the bullet tasks look at
static void** const th11_pause_object = (void**)0x004A8E88;
static const size_t th11_pause_flags = 0x60;

// The pool holds 2001 bullets, so the lists of the ones in play can't be longer
static const size_t th11_bullets_saved_max = 2001;
struct Th11SavedBullet {
	void* bullet;
	float position[3];
};
static Th11SavedBullet th11_saved_bullets[th11_bullets_saved_max];
static size_t th11_saved_bullets_count;

// What the last game frame had in play and how much of it was projected forward, for the
// diagnostics dump below
static unsigned int th11_bullets_in_play;
static unsigned int th11_bullets_moved;

// Whether the pause screen has the picture held. A picture the game isn't updating (or isn't
// drawing) must not be moved forward either. 0x408EC0 and 0x408EF0 are the game's two bullet
// tasks, and this is the same test each of them makes - plus the one the pool walk makes per
// bullet (a held picture leaves the bullets in their lists but doesn't run them).
static bool th11_bullets_held() {
	void* pause = *th11_pause_object;
	if (pause == nullptr)
		return false;
	const unsigned int flags = *(unsigned int*)((char*)pause + th11_pause_flags);
	if (((flags | (flags >> 2)) & 1) != 0 || (flags & 4) != 0)
		return true;
	return (flags & 2) != 0 && (flags & 0x400) != 0;
}

// How much of a frame's movement a bullet in this state gets (FUN_00408900)
static float th11_bullet_step_factor(unsigned short state) {
	switch (state) {
	case 1:
		return 1.0f;
	case 2:
	case 3:
		return 0.5f;
	default:
		return 0.0f;
	}
}

// Moves every bullet in play forward by t frames and remembers what it changed
static void th11_bullets_advance(float t) {
	th11_saved_bullets_count = 0;
	th11_bullets_in_play = 0;
	th11_bullets_moved = 0;

	void* manager = *th11_bullet_manager;
	if (manager == nullptr || th11_bullets_held())
		return;

	unsigned int frames_per_render = *th11_frames_per_render;
	if (frames_per_render > 2) // the game's own setting only goes to 2
		frames_per_render = 2;

	float scale = *th11_time_scale;
	if (!(scale > 0.0f) || scale > 2.0f)
		scale = 1.0f;

	const float step = t * (float)(frames_per_render + 1) * scale;

	for (size_t column = 0; column < th11_bullet_columns; ++column) {
		void* bullet = *(void**)((char*)manager + th11_bullet_lists + column * 4);
		for (size_t guard = 0; bullet != nullptr && guard < th11_bullets_saved_max; ++guard) {
			void* next = *(void**)((char*)bullet + th11_bullet_next);

			++th11_bullets_in_play;
			const float factor = th11_bullet_step_factor(*(unsigned short*)((char*)bullet + th11_bullet_state));
			if (factor != 0.0f && th11_saved_bullets_count < th11_bullets_saved_max) {
				float* position = (float*)((char*)bullet + th11_bullet_position);
				const float* velocity = (const float*)((char*)bullet + th11_bullet_velocity);
				Th11SavedBullet& saved = th11_saved_bullets[th11_saved_bullets_count++];
				saved.bullet = bullet;

				const float bullet_step = step * factor;
				for (int axis = 0; axis < 3; ++axis) {
					saved.position[axis] = position[axis];
					position[axis] += velocity[axis] * bullet_step;
				}
				++th11_bullets_moved;
			}

			bullet = next;
		}
	}
}

// Puts the positions of every bullet th11_bullets_advance moved back
static void th11_bullets_undo() {
	for (size_t i = 0; i < th11_saved_bullets_count; ++i) {
		const Th11SavedBullet& saved = th11_saved_bullets[i];
		float* position = (float*)((char*)saved.bullet + th11_bullet_position);
		for (int axis = 0; axis < 3; ++axis)
			position[axis] = saved.position[axis];
	}
	th11_saved_bullets_count = 0;
}

// ---------------------------------------------------------------------------
// Frame interpolation, stage 1: the player, its options and its shots
// ---------------------------------------------------------------------------
//
// The player of the stage that is running is at 0x4A8EB4 (0x8D24 bytes, one animation object of
// its own at +0x14 and another at +0x448). Its draw callback (FUN_00431BC0, the task the stage
// build registers) reads the three floats at +0x87C/+0x880/+0x884, copies them into the body
// animation object's drawn position - (player+0x14)+0x3E8, which is player+0x3FC - and hands that
// object to the sprite queue in the same breath, so the renderer reads exactly that position:
// moving the logical position is what moves the player, the same rule th13's player follows.
//
// Everything else the player puts on screen is positioned by the update instead, so those sprites
// are what has to move:
//
//   - the options are 8 slots of 0xE4 bytes at +0x7570. A slot keeps a flag at +0x00 (non-zero
//     while the slot is in use), the position the game smooths as two ints in 1/128 pixel units at
//     +0x5C/+0x60 (the option update at 0x430B64 through 0x430D8B reads exactly those two and hands
//     them to FUN_004563A0), and the two animation objects it wears as handles at +0xB0/+0xB4.
//   - the shots are 256 slots of 0x6C bytes at +0x96C. The spawn (FUN_00433F90) and the per-frame
//     refresh (FUN_00434440) give a slot in use a non-zero countdown at +0x48, the position the
//     game moves at +0x14/+0x18/+0x1C (three floats, in pixels) and the animation object it wears
//     as a handle at +0x4C (plus a second one at +0x50).
//
// An animation object - what the game builds for a sprite - keeps its drawn position at
// +0x3E8/+0x3EC/+0x3F0, which is what the game's own FUN_004563A0 and FUN_00434440 write and what
// the queue bake (FUN_00451EF0 and the renderers it dispatches to) reads. A handle is the object's
// own address, and the object is threaded onto one of the sprite manager's two lists (at
// [0x4C3268]+0x7B562C and +0x7B5634, next at node+4), which is how FUN_004561E0 looks one up; the
// same walk is written out here rather than calling it, because its arguments are split between a
// register and the stack.
//
// None of the three have a velocity that could be read - the options follow the player and the
// shots follow whatever pattern they were fired with - so what each of them moves by in a frame is
// measured, the way th12 and th13 measure theirs: every game frame the position is compared with
// the one of the frame before, and the extra presentations move the sprite by that difference
// times the fraction of a frame they stand for. The shift is undone after every presentation, so
// what gets measured is the game's own movement. The sprites are looked up again every frame, for
// the reason th12 has to do the same: the game frees them and a slot can keep pointing at one that
// is gone.

static void** const th11_player = (void**)0x004A8EB4;
static const size_t th11_player_position = 0x87C;        // the player's own position, 3 floats
static const size_t th11_player_options = 0x7570;        // 8 slots of 0xE4 bytes
static const size_t th11_player_option_count = 8;
static const size_t th11_player_option_size = 0xE4;
static const size_t th11_player_option_position = 0x5C;  // 2 ints, in 1/128 pixels
static const size_t th11_player_option_sprite = 0xB0;    // 2 animation objects
static const size_t th11_player_shots = 0x96C;           // 256 slots of 0x6C bytes
static const size_t th11_player_shot_count = 256;
static const size_t th11_player_shot_size = 0x6C;
static const size_t th11_player_shot_live = 0x48;        // non-zero while the slot is in use
static const size_t th11_player_shot_position = 0x14;    // 3 floats
static const size_t th11_player_shot_sprite = 0x4C;      // 2 animation objects
static const size_t th11_sprite_position = 0x3E8;        // in an animation object: 3 floats
static const size_t th11_sprite_children = 0x14;         // node[0] = child, node[4] = next
static const size_t th11_sprite_children_off = 0x18;     // non-zero: the children are not moved
// The option positions are kept in 1/128 pixel units
static const float th11_units_per_pixel = 128.0f;
static const float th11_max_step = 48.0f;

// What one thing moved by in a frame, kept per thing the player owns. Every one of them is a fixed
// slot of the player object (the player itself, one of the 8 options, one of the 256 shots), so a
// record never has to be recycled: an empty shot slot simply has nothing to move.
struct Th11Motion {
	unsigned int frame;
	float last_x, last_y;
	float delta_x, delta_y;
};

static unsigned int th11_motion_frame = 1;

static void th11_measure(Th11Motion& motion, float x, float y) {
	// Only a thing that was measured in the frame right before this one has something to compare
	// against - one that just appeared has not moved as far as this can tell
	if (motion.frame + 1 == th11_motion_frame) {
		const float dx = x - motion.last_x;
		const float dy = y - motion.last_y;
		if (dx > -th11_max_step && dx < th11_max_step && dy > -th11_max_step && dy < th11_max_step) {
			motion.delta_x = dx;
			motion.delta_y = dy;
		} else {
			motion.delta_x = 0.0f;
			motion.delta_y = 0.0f;
		}
	} else {
		motion.delta_x = 0.0f;
		motion.delta_y = 0.0f;
	}
	motion.frame = th11_motion_frame;
	motion.last_x = x;
	motion.last_y = y;
}

// The animation object a handle names, or null if it names none that is live right now. An object
// keeps its own address in its first dword and is threaded onto one of the sprite manager's two
// lists. This walks them the way the game's own lookup (FUN_004561E0) does, instead of trusting
// the handle to be a pointer - a slot can keep a stale handle after its sprite is gone, and
// dereferencing that would be a crash.
static void* th11_sprite_by_handle(unsigned int handle) {
	if (handle == 0)
		return nullptr;
	void* manager = *(void**)0x004C3268;
	if (manager == nullptr)
		return nullptr;
	const size_t lists[2] = { 0x7B562C, 0x7B5634 };
	for (size_t list = 0; list < 2; ++list) {
		void* node = *(void**)((char*)manager + lists[list]);
		for (size_t guard = 0; node != nullptr && guard < 0x2000; ++guard) {
			void* object = *(void**)node;
			if (*(unsigned int*)object == handle)
				return object;
			node = *(void**)((char*)node + 4);
		}
	}
	return nullptr;
}

// One thing to move for an extra presentation, and what its position was
struct Th11Shifted {
	float* position;
	float saved[3];
};

static const size_t th11_shifted_max = 4096;
static Th11Shifted th11_shifted[th11_shifted_max];
static size_t th11_shifted_count;

// Moves one position and remembers what it changed
static void th11_shift_position(float* position, const Th11Motion& motion, float t) {
	if (position == nullptr || th11_shifted_count >= th11_shifted_max)
		return;
	Th11Shifted& shifted = th11_shifted[th11_shifted_count++];
	shifted.position = position;
	for (int axis = 0; axis < 3; ++axis)
		shifted.saved[axis] = position[axis];
	position[0] += motion.delta_x * t;
	position[1] += motion.delta_y * t;
}

// The same, for the drawn position of one animation object
static void th11_shift_object(void* object, const Th11Motion& motion, float t) {
	if (object == nullptr)
		return;
	th11_shift_position((float*)((char*)object + th11_sprite_position), motion, t);
}

// The same, for a sprite the game wears: the game gives the objects hanging off it (the node chain
// at +0x14) the same position unless the flag at +0x18 says not to (FUN_004563A0), so they move
// with it here as well.
static void th11_shift_sprite(void* sprite, const Th11Motion& motion, float t) {
	th11_shift_object(sprite, motion, t);
	if (*(const int*)((char*)sprite + th11_sprite_children_off) != 0)
		return;
	void* node = *(void**)((char*)sprite + th11_sprite_children);
	for (size_t guard = 0; node != nullptr && guard < 16; ++guard) {
		th11_shift_object(*(void**)node, motion, t);
		node = *(void**)((char*)node + 4);
	}
}

static void th11_undo_shifts() {
	for (size_t i = 0; i < th11_shifted_count; ++i) {
		const Th11Shifted& shifted = th11_shifted[i];
		for (int axis = 0; axis < 3; ++axis)
			shifted.position[axis] = shifted.saved[axis];
	}
	th11_shifted_count = 0;
}

// The player itself: a motion record of its own, and nothing else to keep
static Th11Motion th11_body_motion;

// The options: what each of the 8 slots moved by, and the animation objects it wears
static Th11Motion th11_option_motions[th11_player_option_count];
static void* th11_option_sprites[th11_player_option_count][2];

// The shots: the same, one per slot of the 256
static Th11Motion th11_shot_motions[th11_player_shot_count];
static void* th11_shot_sprites[th11_player_shot_count][2];

// What the last game frame had on screen and how much of it was projected forward, for the
// diagnostics dump below
static unsigned int th11_sprites_untracked;
static unsigned int th11_body_sprites;
static unsigned int th11_option_sprites_moved;
static unsigned int th11_shot_sprites_moved;
static float th11_player_movement;

// Collects what the player owns this frame and works out what it moved by since the frame before.
// This runs once per game frame, from the game's own present (see th11_present_hook).
static void th11_player_measure() {
	++th11_motion_frame;
	th11_sprites_untracked = 0;
	th11_body_sprites = 0;
	th11_option_sprites_moved = 0;
	th11_shot_sprites_moved = 0;

	void* player = *th11_player;
	if (player == nullptr)
		return;

	for (unsigned int i = 0; i < th11_player_shot_count; ++i) {
		const char* slot = (const char*)player + th11_player_shots + i * th11_player_shot_size;
		if (*(const int*)(slot + th11_player_shot_live) != 0) {
			const float* position = (const float*)(slot + th11_player_shot_position);
			th11_measure(th11_shot_motions[i], position[0], position[1]);
			for (int sprite = 0; sprite < 2; ++sprite) {
				const unsigned int handle =
					*(const unsigned int*)(slot + th11_player_shot_sprite + sprite * 4);
				th11_shot_sprites[i][sprite] = th11_sprite_by_handle(handle);
				if (handle != 0 && th11_shot_sprites[i][sprite] == nullptr)
					++th11_sprites_untracked;
			}
		} else {
			th11_shot_sprites[i][0] = nullptr;
			th11_shot_sprites[i][1] = nullptr;
			th11_shot_motions[i].frame = 0;
		}
	}

	for (unsigned int i = 0; i < th11_player_option_count; ++i) {
		const char* slot = (const char*)player + th11_player_options + i * th11_player_option_size;
		const int* position = (const int*)(slot + th11_player_option_position);
		if (*(const int*)slot == 0) {
			th11_option_sprites[i][0] = nullptr;
			th11_option_sprites[i][1] = nullptr;
			th11_option_motions[i].frame = 0;
			continue;
		}
		th11_measure(th11_option_motions[i],
			(float)position[0] / th11_units_per_pixel, (float)position[1] / th11_units_per_pixel);
		for (int sprite = 0; sprite < 2; ++sprite) {
			const unsigned int handle =
				*(const unsigned int*)(slot + th11_player_option_sprite + sprite * 4);
			th11_option_sprites[i][sprite] = th11_sprite_by_handle(handle);
			if (handle != 0 && th11_option_sprites[i][sprite] == nullptr)
				++th11_sprites_untracked;
		}
	}

	// The player itself: its position is what the update moves, and its draw copies it into the
	// sprite it wears, so that is what is measured
	{
		const float* position = (const float*)((const char*)player + th11_player_position);
		th11_measure(th11_body_motion, position[0], position[1]);
		const float x = th11_body_motion.delta_x < 0.0f ? -th11_body_motion.delta_x : th11_body_motion.delta_x;
		const float y = th11_body_motion.delta_y < 0.0f ? -th11_body_motion.delta_y : th11_body_motion.delta_y;
		const float magnitude = x > y ? x : y;
		if (magnitude > th11_player_movement)
			th11_player_movement = magnitude;
	}
}

// Moves the player and everything it put on screen t frames ahead of the state the game is in, and
// remembers what it changed
static void th11_player_advance(float t) {
	th11_body_sprites = 0;
	th11_option_sprites_moved = 0;
	th11_shot_sprites_moved = 0;
	th11_shifted_count = 0;

	if (th11_bullets_held())
		return;

	void* player = *th11_player;
	if (player == nullptr)
		return;

	if (th11_motion_frame == th11_body_motion.frame) {
		// The draw copies this into the body animation object, and it runs below, so the position
		// the extra presentation draws is this one
		th11_shift_position((float*)((char*)player + th11_player_position), th11_body_motion, t);
		++th11_body_sprites;
	}

	for (unsigned int i = 0; i < th11_player_option_count; ++i) {
		if (th11_motion_frame != th11_option_motions[i].frame)
			continue;
		for (int sprite = 0; sprite < 2; ++sprite) {
			if (th11_option_sprites[i][sprite] == nullptr)
				continue;
			th11_shift_sprite(th11_option_sprites[i][sprite], th11_option_motions[i], t);
			++th11_option_sprites_moved;
		}
	}

	for (unsigned int i = 0; i < th11_player_shot_count; ++i) {
		if (th11_motion_frame != th11_shot_motions[i].frame)
			continue;
		for (int sprite = 0; sprite < 2; ++sprite) {
			if (th11_shot_sprites[i][sprite] == nullptr)
				continue;
			th11_shift_sprite(th11_shot_sprites[i][sprite], th11_shot_motions[i], t);
			++th11_shot_sprites_moved;
		}
	}
}

// ---------------------------------------------------------------------------
// Frame interpolation, stage 1: the straight lasers
// ---------------------------------------------------------------------------
//
// The lasers of a stage live in a manager of their own - [0x4A8E94], 0x47C bytes, allocated by
// FUN_00424B60 and filled in by FUN_00424970 - and the manager's two tasks both walk one list: the
// update task (thunk 0x424D70, walk FUN_00424C10) and the draw task (thunk 0x424DD0, walk
// 0x424D40), which starts from the root node's next at [manager+0x18] and follows each laser's own
// next at +0x08 (its previous is at +0x04). The draw skips a laser whose state at +0x0C is 1, and
// the objects are created by FUN_00424DF0, which threads a new one in at the manager's tail
// (manager+0x450) and counts the ones in play at manager+0x454 up to 0x100.
//
// Which lasers are straight is a whitelist of their vtables, taken from the RTTI the game carries:
// LaserLineInf (0x494A64) and LaserInfiniteInf (0x494ABC) are in it, and the manager's own base
// class (LaserDataInf, 0x494A0C) is left out. th11 has no curve or beam laser class at all - the
// only names in its RTTI are those three - so there is nothing else to leave out.
//
// Each class keeps what it moves by in its own places, which is why the layout is picked by vtable
// along with which classes are straight at all - both were read out of the class's own update
// (LaserLineInf's is FUN_00425CC0, LaserInfiniteInf's is FUN_004272C0), where each of them
// multiplies one of its own fields by the global 0x4A7948 and adds it to the field it belongs to:
//
//   LaserLineInf      velocity +0x48/+0x4C/+0x50, the length grows by the rate at +0x60 up to
//                     +0x450, and it does not turn on its own - its update never writes the angle.
//   LaserInfiniteInf  velocity +0x44C/+0x450/+0x454, the angle turns by +0x45C and the length
//                     grows by the same +0x60 up to +0x460.
//
// Both classes keep the position at +0x3C/+0x40/+0x44, the angle at +0x54 and the length at +0x58,
// and the draw (LaserLineInf's is FUN_00426160, at vtable+0x0C) reads the position and the angle
// out of them and writes them into the two animation objects it embeds - at +0x624 and +0xA58,
// whose drawn positions are at +0x3DC of each - right before it hands them to the sprite queue. So
// moving the laser's own position and angle is what moves the beam on screen.
//
// The length is the one that doesn't come along: the drawn size of the beam is a sprite field that
// the update computes from the length once per frame, and the draw reads nothing of the sort. The
// length is still carried here (it is what the game's own update moves, and keeping it consistent
// keeps the answer the same if the game ever reads it at draw time), but the beam grows in whole
// frames rather than smoothly, the way it does in the game itself.

// One class of straight laser: where it keeps what it moves by, and what it grows towards
struct Th11LaserLayout {
	const void* vtable;
	size_t velocity;        // 3 floats
	size_t angle_speed;     // 0 for a class that doesn't turn
	size_t length_speed;
	size_t length_limit;
};

static void** const th11_laser_manager = (void**)0x004A8E94;
static const size_t th11_laser_list = 0x18;      // in the manager: the first laser of the list
static const size_t th11_laser_next = 0x08;      // in a laser: the next one
static const size_t th11_laser_state = 0x0C;     // in a laser: 1 while it isn't drawn
static const size_t th11_laser_position = 0x3C;  // where it starts, 3 floats
static const size_t th11_laser_angle = 0x54;     // the direction it points in, radians
static const size_t th11_laser_length = 0x58;    // how far it reaches

static const size_t th11_lasers_max = 0x100 + 1; // the manager's own limit, plus its root entry
static const Th11LaserLayout th11_laser_layouts[] = {
	// LaserLineInf: it doesn't turn on its own, and its length stops at +0x450
	{ (const void*)0x00494A64, 0x48,  0,     0x60, 0x450 },
	// LaserInfiniteInf: turns by +0x45C, and its length stops at +0x460
	{ (const void*)0x00494ABC, 0x44C, 0x45C, 0x60, 0x460 },
};

// The layout of the class this laser is, or null if it isn't one of the straight ones
static const Th11LaserLayout* th11_laser_layout(const void* laser) {
	const void* vtable = *(const void* const*)laser;
	for (size_t i = 0; i < sizeof(th11_laser_layouts) / sizeof(th11_laser_layouts[0]); ++i) {
		if (vtable == th11_laser_layouts[i].vtable)
			return &th11_laser_layouts[i];
	}
	return nullptr;
}

struct Th11LaserMoved {
	void* laser;
	float position[3];
	float angle, length;
};

static Th11LaserMoved th11_lasers_moved[th11_lasers_max];
static size_t th11_lasers_moved_count;
static unsigned int th11_lasers_seen;
static unsigned int th11_lasers_projected;

// Moves the straight lasers t frames ahead of the state the game is in, and remembers what it
// changed. Each laser is put where the velocity it carries right now would have it t frames from
// here: the game's own update adds velocity * the global scale to the position once per frame, so
// this is that same step, taken for the fraction of a frame this presentation stands for.
static void th11_lasers_advance(float t) {
	th11_lasers_moved_count = 0;
	th11_lasers_seen = 0;
	th11_lasers_projected = 0;

	void* manager = *th11_laser_manager;
	if (manager == nullptr || th11_bullets_held())
		return;

	unsigned int frames_per_render = *th11_frames_per_render;
	if (frames_per_render > 2) // the game's own setting only goes to 2
		frames_per_render = 2;

	float scale = *th11_time_scale;
	if (!(scale > 0.0f) || scale > 2.0f)
		scale = 1.0f;

	const float step = t * (float)(frames_per_render + 1) * scale;

	void* laser = *(void**)((char*)manager + th11_laser_list);
	// The list is the game's own and this runs where nothing can change it; the bound is only so a
	// list that got broken some other way can't run away with the frame
	for (size_t guard = 0; laser != nullptr && guard < th11_lasers_max * 4; ++guard) {
		void* next = *(void**)((char*)laser + th11_laser_next);
		const Th11LaserLayout* layout = th11_laser_layout(laser);
		// One whose state is 1 isn't drawn yet (the draw walk skips those)
		if (layout != nullptr && *(const int*)((char*)laser + th11_laser_state) != 1) {
			++th11_lasers_seen;
			if (th11_lasers_moved_count < th11_lasers_max) {
				Th11LaserMoved& moved = th11_lasers_moved[th11_lasers_moved_count++];
				moved.laser = laser;

				float* position = (float*)((char*)laser + th11_laser_position);
				float* angle = (float*)((char*)laser + th11_laser_angle);
				float* length = (float*)((char*)laser + th11_laser_length);
				for (int axis = 0; axis < 3; ++axis)
					moved.position[axis] = position[axis];
				moved.angle = *angle;
				moved.length = *length;

				const float* velocity = (const float*)((char*)laser + layout->velocity);
				for (int axis = 0; axis < 3; ++axis)
					position[axis] += velocity[axis] * step;

				if (layout->angle_speed != 0) {
					*angle += *(const float*)((char*)laser + layout->angle_speed) * step;
					// The game keeps the angle in [0, 2 pi) and hands it out from there
					while (*angle < 0.0f)
						*angle += 6.28318531f;
					while (*angle >= 6.28318531f)
						*angle -= 6.28318531f;
				}

				if (layout->length_speed != 0) {
					// Both classes stop growing at the length the game's own update clamps them to
					const float limit = *(const float*)((char*)laser + layout->length_limit);
					*length += *(const float*)((char*)laser + layout->length_speed) * step;
					if (limit > 0.0f && *length > limit)
						*length = limit;
				}
				++th11_lasers_projected;
			}
		}
		laser = next;
	}
}

static void th11_lasers_undo() {
	for (size_t i = 0; i < th11_lasers_moved_count; ++i) {
		const Th11LaserMoved& moved = th11_lasers_moved[i];
		float* position = (float*)((char*)moved.laser + th11_laser_position);
		for (int axis = 0; axis < 3; ++axis)
			position[axis] = moved.position[axis];
		*(float*)((char*)moved.laser + th11_laser_angle) = moved.angle;
		*(float*)((char*)moved.laser + th11_laser_length) = moved.length;
	}
	th11_lasers_moved_count = 0;
}

// ---------------------------------------------------------------------------
// Frame interpolation, stage 0: extra presentations
// ---------------------------------------------------------------------------
//
// th11 draws a game frame the way th12 to th15 do. The block that does it sits in three frame
// functions, one per kind of scene the game can be in, and all three are the same:
//
//     0x446152  mov eax, [0x4C3288]       the device
//     0x446159  [ecx+0xA4]                BeginScene
//     0x446162  [0x4C3268]                reset the sprite queue, inlined here
//     0x44617F  mov edi, 0x4C3280         the render context, for the call below
//     0x446184  [0x4C3C10] = 0xFF
//     0x44618E  FUN_0042A300              prepare the render context (its object in EDI)
//     0x446193  FUN_00456E10              render every object (they fill the sprite queue)
//     0x446198  mov esi, [0x4C3268]       the sprite queue's owner, for the call below
//     0x44619E  FUN_0044FD10              draw the sprite queue (its object in ESI)
//     0x4461AA  [ecx+0x104]               SetTexture(0, NULL)
//     0x4461BC  [ecx+0xA8]                EndScene
//
// The other two blocks are at 0x446530 and 0x4466EF, with the same instructions at different
// addresses (in FUN_00446420, the window update, and FUN_00446650). Each frame function then calls
// a helper of its own right after its EndScene, and that is where it presents: 0x446382 in
// FUN_004461F0, 0x4465D6 in FUN_004465D0 and 0x4467F9 in FUN_00446790. Every one of them is a 5
// byte load of the device followed by the arguments being pushed and the present called over the
// 16 bytes after it.
//
// The sprite queue is what the game's own sprites are queued in before they are drawn in one batch:
// its owner is [0x4C3268], the queue itself is a buffer inside it at +0x435624 with the number of
// queued sprites at +0x435620 and two pointers to its ends at +0x7B5624/+0x7B5628 (FUN_0044FCE0
// resets it, FUN_0044FD10 draws it and resets it again). The frame function inlines the four stores
// the reset does instead of calling it, and the extra presentations below do the same through the
// function, which is the same four stores.
//
// Two of the calls take their object in a register instead of on the stack, which is how the
// compiler that built this game left them, so they can't be reached through a function pointer -
// the trampolines below are what put the object where each one expects it. Both registers are
// callee-saved and are put back before returning: leaving them holding the object hands the caller
// something other than what it had, which is what broke the th13 version of this code (it crashed
// in the SetTexture right after the sprite queue was drawn).
//
// The extra presentations draw the state the game is already in, which is the "moved forward by
// zero frames" case; stage 1 is what puts anything of the game's own in motion inside them.

static void* const th11_prepare_render_address = (void*)0x0042A300;
static void* const th11_draw_sprite_queue_address = (void*)0x0044FD10;

static void __declspec(naked) th11_prepare_render() {
	__asm {
		push edi
		mov edi, 0x004C3280
		call dword ptr [th11_prepare_render_address]
		pop edi
		ret
	}
}

static void __declspec(naked) th11_draw_sprite_queue() {
	__asm {
		push esi
		mov esi, dword ptr ds:[0x004C3268]
		call dword ptr [th11_draw_sprite_queue_address]
		pop esi
		ret
	}
}

// The plain calls the render block makes into the game
static auto th11_reset_sprite_queue = (void(*)())0x0044FCE0;
static auto th11_render_objects = (int(*)())0x00456E10;

// Where the user's tool builds its frame and where it draws it. thprac's th11 module starts its
// frame from a hook on the return of the scene update at 0x456DEB and draws the overlay it built
// from a hook on the return of the object render at 0x456F12 (see tool_state.h); both are the ret
// of the walk the render block above calls into.
static unsigned char* const th11_tool_frame_address = (unsigned char*)0x00456DEB;
static unsigned char* const th11_tool_draw_address = (unsigned char*)0x00456F12;

// What the scene is drawn with, taken from the frame the game's own render ran in. Each of the
// three frame functions, right before it renders, makes its 2D context the current one (0x4C359C
// into [0x4C37CC]), sets the viewport from it (FUN_00446080 at 0x4460D6 to 0x4460F1) and calls
// FUN_0042A810 on it, which builds the view and projection matrices in the context (at +0x4C and
// +0x8C) and hands them to SetTransform, and copies the two values at +0xE8/+0xEC of it into the
// sprite manager (at +0xB0/+0xB4) - those are what places the sprite queue on screen. So what an
// extra presentation has to put back is whatever the game's frame left: the user's tool draws its
// overlay with a viewport, a projection and a screen placement of its own, and an extra
// presentation runs after that.
static D3DVIEWPORT9 th11_scene_viewport = {};
static D3DMATRIX th11_scene_view = {};
static D3DMATRIX th11_scene_projection = {};
static bool th11_scene_transforms = false;
static void* const th11_render_context = (void*)0x004C37CC;  // the current 2D context
static void* const th11_sprite_manager = (void*)0x004C3268;
static DWORD th11_scene_context;
static DWORD th11_scene_sprite_offset[2];
static const size_t th11_sprite_screen_offset = 0xB0;        // in the sprite manager

// The device the game draws with. The present sites load it from the game's own global, so that is
// the device that has to present the frame - not the one the patch's D3D9 hook captured, which is
// the same object while that hook is in place and nothing at all if the game got its device in a
// way the hook doesn't see. Reporting a failed present to the game ends it (its device-lost
// handling takes over), so the device the game itself would have presented with comes first.
static IDirect3DDevice9* th11_device() {
	IDirect3DDevice9* device = *(IDirect3DDevice9**)0x004C3288;
	if (device != nullptr)
		return device;
	return d3d9_hooked_device();
}

// The menus draw their text through a list of records the game keeps in the text object it holds at
// [0x4A8D58]: the game's own printf-for-text call appends a record (0x130 bytes each, at +0x87C -
// FUN_004014E0 does that, and FUN_004015A0 is the vsprintf wrapper in front of it) and bumps the
// frame's count of them (+0x1847C), and the drawing then walks the records that count covers
// (FUN_00401670, whose loop at 0x401C56 compares its index with that count). The list is worked
// over once per frame - FUN_004014A0, the compaction that drops the records whose timer ran out, is
// part of what the update walk does, which an extra presentation does not run - so an extra frame
// that draws the same screen while the list still holds the records the game's own frame appended
// draws that text a second time on top of itself, which the alpha blending these screens use turns
// into text that looks a little denser than the game's own frame shows.
//
// So the count the game's own run of the walk started from is kept here and put back before every
// extra presentation: each one then appends its own records from the start of the list, exactly the
// way the game's own frame did.
static const size_t th11_text_record_count = 0x1847C;
static void* const th11_text_object = (void*)0x004A8D58;
static unsigned int th11_text_records;

static unsigned int th11_text_count() {
	void* text = *(void**)th11_text_object;
	return text != nullptr ? *(unsigned int*)((char*)text + th11_text_record_count) : 0;
}

static void th11_text_records_save() {
	th11_text_records = th11_text_count();
}

static void th11_text_records_restore() {
	void* text = *(void**)th11_text_object;
	if (text != nullptr)
		*(unsigned int*)((char*)text + th11_text_record_count) = th11_text_records;
}

// Remembers the viewport the scene is about to be drawn with, then lets the game's own render run
// exactly as it always did (see th11_extra_presentation for what it's for)
//
// The screen shake: the game writes the offset it shakes the picture by into the Float2 at 0x4C379C
// (x) and 0x4C37A0 (y) while the stage is drawn (0x4487B4 and 0x455327) and clears it inside the
// same pass - the diagnostics caught 0x4C379C going from -3.0 to 0 and 0x4C37A0 from 2.0 to 0, and
// the drawing paths read them at 0x403173 and 0x4035D4. An extra pass that starts after that clear
// draws the frame unshaken, so the values the game's own pass started with are put back around every
// extra pass (0x4C37A4 is the third float of the same vector).
static ExtraFloatState th11_shake = { (float*)0x004C379C, 3 };

// ---------------------------------------------------------------------------
// Frame interpolation: the stage object's transition state
// ---------------------------------------------------------------------------
//
// As in th10, part of the stage's transition is driven from its draw callbacks. FUN_004026xx
// registers three tasks on the stage object at [0x4A8D60] (and its second slot [0x4A8D5C]): the
// update one (0x403900, key 0xC) through FUN_00456B70, and two draw ones (0x403910 at key 2 and
// 0x403920 at key 5) through FUN_00456C10. The draw ones change the object:
//
//   0x40383D (FUN_004035A0): if ([stage+0x2FF8] > 0) FUN_00459210(&[stage+0x2FF4], -1.0f), which
//                            subtracts one from the stage clear timer
//   0x40335B (FUN_00403130): if ([stage+0x2FF0] & 4 and [stage+0x2FF8] < 30)
//                            FUN_004489E0(3, 30, 0, 0, 0x0A), which creates a 30 frame fade
//
// The game draws once a frame, so those callbacks run once a frame. An extra presentation runs the
// render chain again, which subtracts the timer two or three times per game frame and can create
// more than one fade for the same frame - the same bug th10 had. The fade object itself is fine:
// its own timer runs on the update chain, once a frame.
//
// So an extra pass copies both stage objects (0x3158 bytes, the size FUN_00402470/FUN_004029E2
// allocate) before its render walk and puts them back afterwards, and the fade factory call at
// 0x40335B is skipped while an extra is drawing.
static const size_t th11_stage_size = 0x3158;
static void** const th11_stage_slots[2] = { (void**)0x004A8D5C, (void**)0x004A8D60 };
static unsigned char* th11_stage_saved[2];
static bool th11_stage_saved_valid[2];
static bool th11_in_extra;

static void th11_stage_save() {
	for (int i = 0; i < 2; ++i) {
		th11_stage_saved_valid[i] = false;
		void* stage = *th11_stage_slots[i];
		if (stage == nullptr)
			continue;
		if (th11_stage_saved[i] == nullptr)
			th11_stage_saved[i] = (unsigned char*)malloc(th11_stage_size);
		if (th11_stage_saved[i] == nullptr)
			continue;
		memcpy(th11_stage_saved[i], stage, th11_stage_size);
		th11_stage_saved_valid[i] = true;
	}
}

static void th11_stage_restore() {
	for (int i = 0; i < 2; ++i) {
		void* stage = *th11_stage_slots[i];
		if (!th11_stage_saved_valid[i] || stage == nullptr)
			continue;
		memcpy(stage, th11_stage_saved[i], th11_stage_size);
		th11_stage_saved_valid[i] = false;
	}
}

// FUN_004489E0 builds a fade object and puts its tasks on the chains. The call from the stage's
// draw callback at 0x40335B is the one that creates the stage clear fade; an extra presentation
// must not create another one, which is what this wrapper does while th11_in_extra is set. The
// callers ignore the return value, so nullptr is safe; everything else goes to the game's factory.
static void* (__stdcall* th11_fade_create)(int mode, int duration, int a3, int a4, int a5) =
	(void*(__stdcall*)(int, int, int, int, int))0x004489E0;

static void* __stdcall th11_fade_create_extra_guard(int mode, int duration, int a3, int a4, int a5) {
	if (th11_in_extra)
		return nullptr;
	return th11_fade_create(mode, duration, a3, a4, a5);
}

static void th11_render_hook() {
	IDirect3DDevice9* device = th11_device();
	if (device != nullptr) {
		device->GetViewport(&th11_scene_viewport);
		th11_scene_transforms = SUCCEEDED(device->GetTransform(D3DTS_VIEW, &th11_scene_view)) &&
			SUCCEEDED(device->GetTransform(D3DTS_PROJECTION, &th11_scene_projection));
	}
	th11_scene_context = *(DWORD*)th11_render_context;
	void* manager = *(void**)th11_sprite_manager;
	if (manager != nullptr) {
		th11_scene_sprite_offset[0] = *(DWORD*)((char*)manager + th11_sprite_screen_offset);
		th11_scene_sprite_offset[1] = *(DWORD*)((char*)manager + th11_sprite_screen_offset + 4);
	}

	ToolState::WatchFrameReady(th11_tool_frame_address, th11_tool_draw_address);

	// What the game's own run of the walk finds the text list at, for the extra presentations to put
	// back (see th11_text_record_count)
	th11_text_records_save();

	extra_float_state_save(&th11_shake);

	th11_render_objects();
}

// Diagnostics: press U to write the presentation timing of the last frames to oilp_diag.txt
static const unsigned int th11_diag_frames_max = 300;
static const unsigned int th11_diag_presents_max = 8;
struct Th11DiagFrame {
	LARGE_INTEGER base;
	unsigned int presents;
	LARGE_INTEGER present_time[th11_diag_presents_max];
};
static Th11DiagFrame th11_diag_frames[th11_diag_frames_max];
static unsigned int th11_diag_count;
static bool th11_diag_key_down;

static LARGE_INTEGER th11_perf_freq;

static void th11_diag_dump() {
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

	unsigned int count = th11_diag_count < th11_diag_frames_max ? th11_diag_count : th11_diag_frames_max;
	unsigned int start = th11_diag_count < th11_diag_frames_max ? 0 : th11_diag_count % th11_diag_frames_max;

	fprintf(file, "oilp frame interpolation diagnostics\n");
	fprintf(file, "logic frame rate: %.2f, configured presentation rate: %.2f\n",
		Limiter::game_fps, Limiter::PresentRate());
	fprintf(file, "frames recorded: %u\n", count);
	fprintf(file, "bullets: %u in play, %u projected forward (last game frame)\n",
		th11_bullets_in_play, th11_bullets_moved);
	fprintf(file, "player: %u body, %u option and %u shot sprite(s) projected (last game frame)\n",
		th11_body_sprites, th11_option_sprites_moved, th11_shot_sprites_moved);
	fprintf(file, "player: %u sprite(s) this frame had no movement record (should stay 0)\n",
		th11_sprites_untracked);
	fprintf(file, "player movement: up to %.2f px per frame since the last dump\n", th11_player_movement);
	th11_player_movement = 0.0f;
	fprintf(file, "lasers: %u in play, %u projected forward (last game frame)\n",
		th11_lasers_seen, th11_lasers_projected);

	double presents = 0.0;
	double first = 0.0;
	double last = 0.0;
	for (unsigned int i = 0; i < count; ++i) {
		const Th11DiagFrame& frame = th11_diag_frames[(start + i) % th11_diag_frames_max];
		fprintf(file, "frame %4u: %u present(s)", i, frame.presents);
		for (unsigned int p = 0; p < frame.presents && p < th11_diag_presents_max; ++p)
			fprintf(file, " [%u] %.2fms", p,
				(double)(frame.present_time[p].QuadPart - frame.base.QuadPart) * 1000.0 / (double)th11_perf_freq.QuadPart);
		fprintf(file, "\n");
		presents += frame.presents;
		if (i == 0)
			first = (double)frame.base.QuadPart;
		last = (double)frame.base.QuadPart;
	}
	if (last > first)
		fprintf(file, "average: %.3f presents per frame, %.1f presentations per second\n",
			presents / (double)count, presents * (double)th11_perf_freq.QuadPart / (last - first));

	fclose(file);
	printf("Wrote oilp_diag.txt\n");
}

// One extra presentation of the frame: the same render block the game runs, once more, so the state
// the game is in is drawn and presented again. The tool's frame is readied again, so the object
// render below draws its overlay into this presentation the same way it does in the game's own one.
static void th11_extra_presentation(IDirect3DDevice9* device, float t) {
	device->BeginScene();
	if (th11_scene_transforms) {
		device->SetTransform(D3DTS_VIEW, &th11_scene_view);
		device->SetTransform(D3DTS_PROJECTION, &th11_scene_projection);
	}
	if (th11_scene_viewport.Width != 0)
		device->SetViewport(&th11_scene_viewport);
	// The 2D context the game's own frame rendered with, and the screen placement it left in the
	// sprite queue (see th11_scene_viewport)
	*(DWORD*)th11_render_context = th11_scene_context;
	{
		void* manager = *(void**)th11_sprite_manager;
		if (manager != nullptr) {
			*(DWORD*)((char*)manager + th11_sprite_screen_offset) = th11_scene_sprite_offset[0];
			*(DWORD*)((char*)manager + th11_sprite_screen_offset + 4) = th11_scene_sprite_offset[1];
		}
	}
	th11_reset_sprite_queue();
	*(DWORD*)0x004C3C10 = 0xFF;
	th11_prepare_render();
	ToolState::ReadyToDraw();
	// The text list goes back to how the game's own run of the walk found it, so this presentation
	// draws the same text that one drew (see th11_text_record_count above)
	th11_text_records_restore();
	// Everything is drawn where it will be t frames from now; the render bakes those positions into
	// what it draws, so they can all go back right after it
	th11_bullets_advance(t);
	th11_player_advance(t);
	th11_lasers_advance(t);
	// The stage's draw callbacks advance its transition (see th11_stage_save); put the object back
	// afterwards so an extra pass cannot move the transition a second or third time
	th11_stage_save();
	th11_in_extra = true;
	// Put the shake back for this pass (see the comment above)
	extra_float_state_begin(&th11_shake);
	th11_render_objects();
	extra_float_state_end(&th11_shake);
	th11_in_extra = false;
	th11_stage_restore();
	th11_lasers_undo();
	th11_bullets_undo();
	th11_undo_shifts();
	th11_draw_sprite_queue();
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
static HRESULT th11_present_hook() {
	IDirect3DDevice9* device = th11_device();
	if (device == nullptr)
		return E_FAIL;

	static bool freq_queried = false;
	if (!freq_queried) {
		freq_queried = true;
		QueryPerformanceFrequency(&th11_perf_freq);
	}

	LARGE_INTEGER base;
	QueryPerformanceCounter(&base);

	// The game's own update has run and it is about to put that frame on screen: this is the one
	// point per frame where the movement of the player, its options and its shots can be measured
	th11_player_measure();

	HRESULT result = device->Present(nullptr, nullptr, nullptr, nullptr);
	overlay_mark_presentation();
	Limiter::MarkFrameWorkEnd();
	ToolState::WatchFrameDrawn();

	unsigned int presents = 1;
	LARGE_INTEGER present_time[th11_diag_presents_max];
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

				if (presents < th11_diag_presents_max)
					QueryPerformanceCounter(&present_time[presents]);
				++presents;

				// Where this presentation sits between the game's own one and the next one:
				// the frames of one game frame are spread evenly over it, so the k-th of
				// extras + 1 of them draws the state k/(extras + 1) frames along
				th11_extra_presentation(device, (float)k / (float)(extras + 1));
			}
		}
	}

	{
		Th11DiagFrame& frame = th11_diag_frames[th11_diag_count % th11_diag_frames_max];
		frame.base = base;
		frame.presents = presents;
		for (unsigned int p = 0; p < th11_diag_presents_max; ++p)
			frame.present_time[p] = p < presents ? present_time[p] : base;
		++th11_diag_count;
	}

	if (GetAsyncKeyState('U') & 0x8000) {
		if (!th11_diag_key_down) {
			th11_diag_key_down = true;
			th11_diag_dump();
		}
	} else {
		th11_diag_key_down = false;
	}

	return result;
}

// Jumps over the present the game sets up after its present call site, so the code after it runs
// with the result the hook left in eax.
static void th11_skip_present_setup(DWORD address, size_t size) {
	BYTE patch[32];
	patch[0] = 0xE9;
	*(DWORD*)(patch + 1) = (address + size) - (address + 5);
	for (size_t i = 5; i < size; ++i)
		patch[i] = 0x90;
	patch_bytes(address, patch, size);
}

void th11_install_patches() {
	{
		// Skip the original frame limiter
		BYTE patch[] = { 0xEB, 0x43 };
		patch_bytes(0x00446454, patch, sizeof(patch));
	}
	{
		// Force fast input latency mode
		BYTE patch[] = { 0xEB };
		patch_bytes(0x00445877, patch, sizeof(patch)); // Skip over automatic
		patch_bytes(0x0044588B, patch, sizeof(patch)); // Skip over normal
	}
	{
		// Hook window update
		patch_call(0x004458A7, th11_window_update_hook);
	}
	if (Config::D3D9Ex) {
		// Redirect Direct3DCreate9 call
		// IAT is being hooked, but thcrap also hooks Direct3DCreate9 in the same place
		// This should force our Direct3DCreate9 hook to be loaded no matter what
		patch_call(0x0044570E, Direct3DCreate9_hook);
	}
	if (Config::ReplaySpeedControl) {
		// Skip the original replay speed control stuff
		BYTE patch[] = { 0xEB };
		patch_bytes(0x00436D5F, patch, sizeof(patch));
	}
	if (Config::Interpolation || Config::InterpolationFPS) {
		// Take over the object render, only to remember the viewport the scene is drawn with (see
		// th11_render_hook); the render itself runs exactly the way it always did
		static const DWORD render_calls[] = { 0x00446193, 0x00446571, 0x00446730 };
		unsigned int render_patched = 0;
		for (DWORD address : render_calls) {
			BYTE* render_call = (BYTE*)address;
			if (render_call[0] == 0xE8) {
				patch_call(address, (void*)th11_render_hook);
				++render_patched;
			} else {
				printf("th11 render call at 0x%x doesn't look like a known one, skipping!\n", address);
			}
		}

		// The stage's draw callback FUN_00403130 creates the stage clear fade at 0x40335B; an
		// extra presentation must skip that (see th11_fade_create_extra_guard)
		BYTE* fade_call = (BYTE*)0x0040335B;
		if (fade_call[0] == 0xE8 &&
			(DWORD)0x00403360 + *(DWORD*)(fade_call + 1) == 0x004489E0) {
			patch_call(0x0040335B, (void*)th11_fade_create_extra_guard);
		} else {
			printf("th11 fade creation call at 0x40335B doesn't look like the known one, skipping!\n");
		}

		// Take over the presents in the three frame functions' helpers. Each site starts by loading
		// the device (5 bytes, exactly what a hooked call needs) and then pushes its arguments and
		// calls Present over the 16 bytes below, which are skipped over so the code after them runs
		// with the result the hook left in eax.
		static const DWORD present_sites[] = { 0x00446382, 0x004465D6, 0x004467F9 };
		unsigned int present_patched = 0;
		for (DWORD address : present_sites) {
			BYTE* present_site = (BYTE*)address;
			if (present_site[0] == 0xA1 && *(DWORD*)(present_site + 1) == 0x004C3288) {
				patch_call(address, (void*)th11_present_hook);
				th11_skip_present_setup(address + 5, 16);
				++present_patched;
			} else {
				printf("th11 present call at 0x%x doesn't look like a known one, skipping!\n", address);
			}
		}
		printf("Frame interpolation: %u render call(s) and %u present call(s) taken over (setting %d)\n",
			render_patched, present_patched,
			Config::InterpolationFPS ? (int)Config::InterpolationFPS : Config::Interpolation);

		// The frame rate the frames are actually presented at goes right after the line the patch
		// draws in the bottom left corner (see overlay.h), the same as in th12 to th15
		// (the overlay readouts are Config::ShowOverlay's business; see Limiter::Initialize)
		overlay_set_present_rate_follow_text(true);
	}
}

FPSTarget th11_replay_callback() {
	if (*CReplayManager::InstancePtr && (*CReplayManager::InstancePtr)->mode == 1) {
		// TODO: Reverse engineer this struct instead of being lazy
		auto input = *(DWORD*)0x004C92A8;
		if (input & InputState::Focus)
			return FPSTarget::ReplaySlow;
		else if (input & (InputState::Skip | InputState::Shoot))
			return FPSTarget::ReplaySkip;
	}
	return FPSTarget::Game;
}
