// Touhou 10: Mountain of Faith v1.00a

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
#include "touhou10.h"

using namespace Touhou10;

CReplayManager** CReplayManager::InstancePtr = (CReplayManager**)0x00477838;

int __stdcall th10_window_update_hook(void* self) {
	Limiter::Tick();
	return CWindowManager__Update(self);
}

// ---------------------------------------------------------------------------
// Frame interpolation, stage 0: extra presentations
// ---------------------------------------------------------------------------
//
// th10 draws a game frame from a single place, unlike the later games, where the same block is
// repeated once per scene: it sits inside the window update itself, CWindowManager__Update at
// 0x439390, which the main loop calls once per frame with the window object at 0x4924F0 (0x438D2C
// pushes it and calls; that call is what the patch replaces with th10_window_update_hook). Every
// screen the game can be in - the title, the menus, a stage - is drawn by this one function, so
// this is also where the menus get their extra presentations.
//
// The frame function starts and ends the scene the way the later games do:
//
//     0x43948E  mov eax, [0x491C30]      the device
//     0x439496  call [ecx+0xA4]          BeginScene
//     0x43949C  ...                      reset the sprite queue, inlined here (three stores)
//     0x4394BD  mov edi, 0x491C28        the render context, for the call below
//     0x4394C2  [0x492378] = 0xFF
//     0x4394CC  FUN_00420CD0             prepare the render context (its object in EDI)
//     0x4394D8  FUN_00449D40             render every object (they fill the sprite queue)
//     0x4394DD  mov esi, [0x491C10]      the sprite queue's owner, for the call below
//     0x4394E3  FUN_00442F50             draw the sprite queue (its object in ESI)
//     0x4394F4  call [ecx+0x104]         SetTexture(0, NULL)
//     0x439502  call [edx+0xA8]          EndScene
//     0x439508  ...                      the frame counter goes back to 0
//     0x43950C  FUN_004391F0             present
//
// The block runs on every call of the function - the counter it is gated by, [0x491D66] plus one
// compared against the window object's +0x14, always passes, because [0x491D66] is a byte that is
// never written anywhere in the executable and starts at 0 (the three places that read it are
// 0x41366A, 0x4208DC and 0x43947B, all reads), the same "one update per rendered frame" the later
// games have. What does gate the block is the value the update walk returns: only 1 renders, so a
// screen that is being torn down or switched presents nothing that frame.
//
// The device is [0x491C30], the sprite queue's owner is [0x491C10], and the queue itself is a
// vertex buffer inside that owner: 0x3ADAC8 is how many sprites it holds, 0x3ADACC is where it
// starts, 0x72DACC is where the next sprite goes and 0x72DAD0 is the start of the batch that is
// drawn (FUN_00442F50 draws from it and then moves it up to the write cursor, which is what makes
// the report of "nothing to draw" a count of 0). The inlined reset is those three stores.
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

static void* const th10_prepare_render_address = (void*)0x00420CD0;
static void* const th10_draw_sprite_queue_address = (void*)0x00442F50;

static void __declspec(naked) th10_prepare_render() {
	__asm {
		push edi
		mov edi, 0x00491C28
		call dword ptr [th10_prepare_render_address]
		pop edi
		ret
	}
}

static void __declspec(naked) th10_draw_sprite_queue() {
	__asm {
		push esi
		mov esi, dword ptr ds:[0x00491C10]
		call dword ptr [th10_draw_sprite_queue_address]
		pop esi
		ret
	}
}

// The call the render block makes into the game: the render walk, which runs every drawing task and
// so fills the sprite queue. It takes the scene object ([0x491BE4]) on the stack and cleans it up
// itself (its `ret 4` is at 0x449E2E).
static auto th10_render_objects = (int(__stdcall*)(void*))0x00449D40;
static void** const th10_scene = (void**)0x00491BE4;

// The update walk, which is what runs the game's own logic (the enemies, the bullets, the player,
// the timers). It takes the scene object and cleans it up itself as well (its `ret 4` is at
// 0x449D0E). Only counted here, so the diagnostics can show how much logic the game ran for a
// frame - the walk itself runs exactly as it always did.
static auto th10_update_objects = (int(__stdcall*)(void*))0x00449C00;
static unsigned int th10_frame_updates;

static int __stdcall th10_update_hook(void* scene) {
	++th10_frame_updates;
	return th10_update_objects(scene);
}

// The stage object and, at +0x38, the timer the stage counts its own frames in (the one thprac
// writes to when it starts stage 4 forward, see its th10 module): a second way to see how fast the
// game's own logic is going, straight out of the game's data.
static void** const th10_stage = (void**)0x004776E8;
static const size_t th10_stage_timer = 0x38;

// Where the user's tool builds its frame and where it draws it. thprac's th10 module starts its
// frame from a hook on the return of the update walk at 0x449D0E and draws the overlay it built
// from a hook on the return of the render walk at 0x449E2E; both are the `ret 4` of the walk the
// frame function calls (FUN_00449C00 and FUN_00449D40), which is why the tool state is told about
// them: 0xC2 is what those two bytes hold while no tool is loaded, and the code there takes the
// scene off the stack when it returns (see tool_state.h).
static unsigned char* const th10_tool_frame_address = (unsigned char*)0x00449D0E;
static unsigned char* const th10_tool_draw_address = (unsigned char*)0x00449E2E;
static const unsigned char th10_tool_plain_byte = 0xC2;

// What the scene is drawn with, taken from the frame the game's own render ran in. The frame
// function, right before it renders, makes its 2D context the current one (0x491E94 into
// [0x491FAC]), calls FUN_00421480 on it - which builds the view and projection matrices in the
// context (at +0x4C and +0x8C), hands them to SetTransform, and copies the two values at
// +0xE8/+0xEC of it into the sprite manager (at +0x5C/+0x60), which is what places the sprite
// queue on screen - and sets the viewport from it (at 0x439419, SetViewport with the context's
// +0xCC). So what an extra presentation has to put back is whatever the game's frame left: the
// user's tool draws its overlay with a viewport, a projection and a screen placement of its own,
// and an extra presentation runs after that.
//
// FUN_00421480 is not called again for the extra presentations for the same reason th11 doesn't
// call its own camera: it draws the sprite queue on its way in before it does anything else (the
// first thing it does is FUN_00442F50 when the sprite manager exists).
static D3DVIEWPORT9 th10_scene_viewport = {};
static D3DMATRIX th10_scene_view = {};
static D3DMATRIX th10_scene_projection = {};
static bool th10_scene_transforms = false;
static void* const th10_render_context = (void*)0x00491FAC;  // the current 2D context
static void* const th10_sprite_manager = (void*)0x00491C10;
static DWORD th10_scene_context;
static DWORD th10_scene_sprite_offset[2];
static const size_t th10_sprite_screen_offset = 0x5C;        // in the sprite manager

// The device the game draws with. The present and render sites load it from the game's own global,
// so that is the device that has to present the frame - not the one the patch's D3D9 hook
// captured, which is the same object while that hook is in place and nothing at all if the game
// got its device in a way the hook doesn't see. Reporting a failed present to the game ends it
// (its device-lost handling takes over), so the device the game itself would have presented with
// comes first.
static IDirect3DDevice9* th10_device() {
	IDirect3DDevice9* device = *(IDirect3DDevice9**)0x00491C30;
	if (device != nullptr)
		return device;
	return d3d9_hooked_device();
}

// The menus draw their text through two lists of records the game keeps in the text object it holds
// at [0x4776E0]: the game's own printf-for-text call appends a record to one of them (0x68 bytes
// each, the first at +0x76C with its count at +0x896C, the second at +0x6F6C with its count at
// +0x8970 - FUN_00401530 appends to the first and FUN_004015C0 to the second, both of them bumping
// the count of theirs), and the drawing then walks the records that count covers (FUN_00401760 for
// the first list, whose loop at 0x4019CB compares its index with [obj+0x896C], and FUN_00401A50 for
// the second, at 0x401C19 with [obj+0x8970]). Both counts are cleared once per frame by
// FUN_004014F0, which is one of the object's own update tasks and so part of what the update walk
// does - which an extra presentation does not run. So an extra frame that draws the same screen
// while the lists still hold the records the game's own frame appended draws that text a second
// time on top of itself, which the alpha blending these screens use turns into text that looks a
// little denser than the game's own frame shows (and, on the screens whose layout state moves
// between records, into a visible second copy).
//
// So the counts the game's own run of the walk started from are kept here and put back before every
// extra presentation: each one then draws the records the game's own frame drew.
static const size_t th10_text_records = 0x896C;
static const size_t th10_text_records_count = 2;
static void* const th10_text_object = (void*)0x004776E0;
static unsigned int th10_text_saved[th10_text_records_count];

static void th10_text_records_save() {
	void* text = *(void**)th10_text_object;
	for (size_t i = 0; i < th10_text_records_count; ++i)
		th10_text_saved[i] = text != nullptr
			? *(unsigned int*)((char*)text + th10_text_records + i * 4) : 0;
}

static void th10_text_records_restore() {
	void* text = *(void**)th10_text_object;
	if (text == nullptr)
		return;
	for (size_t i = 0; i < th10_text_records_count; ++i)
		*(unsigned int*)((char*)text + th10_text_records + i * 4) = th10_text_saved[i];
}

// The game keeps its own frame rate in an object of its own (fps.cpp: FUN_00413350 allocates it and
// puts it at [0x477708], and its task - the draw callback at 0x413690 - is on the render chain, so
// every run of the render walk runs it). The task counts the frames in a one second window (the
// counter at +0x20, advanced by [0x491D66] + 1 every time it runs) and works the rate out from that
// count and the window's own clock (FUN_004134B0), which is what it shows as "%2.1ffps". The rate
// is also acted on: when it goes above the threshold the game has at [0x470BB8] it treats the frame
// as having run too fast and, after a couple of those in a row, re-syncs its own clock from the
// performance counter (and clears that counter's base, which makes its next frame rate read as 0).
//
// An extra presentation is another run of the render walk, so without putting this state back the
// game counts three frames for every one it runs, shows 180, and ends up re-syncing its clock from
// it. The counters are put back the way the text records are: what the game's own frame left is
// kept, and written back around each extra presentation, so the extras are presentations and
// nothing else - they show the rate the game's own frame showed and never count as frames.
static void** const th10_fps = (void**)0x00477708;
static const size_t th10_fps_window = 0x14;       // where the current window started, a double
static const size_t th10_fps_state = 0x14;        // from here (the window start, a double) up to
static const size_t th10_fps_state_size = 0x24;   // +0x38: the window, the "too fast" count, the
                                                  // frame counter, two more doubles and the rate
static unsigned char th10_fps_saved[0x24];
static bool th10_fps_saved_valid;

static void th10_fps_save() {
	void* fps = *th10_fps;
	th10_fps_saved_valid = fps != nullptr;
	if (th10_fps_saved_valid)
		memcpy(th10_fps_saved, (char*)fps + th10_fps_state, th10_fps_state_size);
}

// as_presentation is set when this is being put back for an extra presentation rather than for the
// game's own accounting: the rate the game shows is worked out once a one second window, and the
// game's own pass works it out from the count as it is *before* the pass puts itself into it (the
// bump is the task's last step). A presentation a few milliseconds later would find that window
// over, work the rate out again from the count that now has that pass in it, and show a rate one
// count higher for its one presentation (60.0 turning into 60.7 for five milliseconds, once a
// second - the little flicker in the corner). Its window start is left reading as the future so
// the guard at the top of the rate takes it as "the window starts now", which leaves the elapsed
// at 0: the rate is not worked out again and the value the game's own pass showed stays in place.
static void th10_fps_restore(bool as_presentation) {
	void* fps = *th10_fps;
	if (!th10_fps_saved_valid || fps == nullptr)
		return;
	memcpy((char*)fps + th10_fps_state, th10_fps_saved, th10_fps_state_size);
	if (as_presentation) {
		const double future = 1.0e300;
		memcpy((char*)fps + th10_fps_window, &future, sizeof(future));
	}
}

// ---------------------------------------------------------------------------
// Frame interpolation: the stage object's transition state
// ---------------------------------------------------------------------------
//
// The stage's transition is not driven only by its update callback. Two of the three tasks it
// registers live on the render chain: FUN_00402230 registers FUN_00403060 -> FUN_00402850 at key
// 7 and FUN_00403070 -> FUN_00402CA0 at key 10, and both change its state:
//
//   0x402F88 (FUN_00402CA0): if ([stage+0x2A20] > 0) FUN_0044BF40(-1.0f), which subtracts one
//                            from the stage clear timer at [stage+0x2A1C..0x2A2C]
//   0x402A61 (FUN_00402850): if ([stage+0x2A18] & 4 and [stage+0x2A20] < 30)
//                            FUN_0043C8B0(3, 30, 0, 0, 0), which creates a 30 frame fade to black
//
// The game runs that chain once per frame. An extra presentation runs it again, so with three
// presentations per game frame the stage clear timer is decremented three times (the transition
// runs three times as fast) and the fade factory can add more than one fade task for one game
// frame. The fade object itself is fine: its own timer runs on the update chain, once a frame.
//
// So an extra pass puts the whole stage object back after its render walk, and the fade factory
// call in FUN_00402850 is skipped while an extra is drawing. The object is 0x2B64 bytes
// (FUN_00402640 allocates it); copying it is cheap next to a render pass.
static const size_t th10_stage_size = 0x2B64;
static void** const th10_stage_slots[2] = { (void**)0x004776E4, (void**)0x004776E8 };
static unsigned char* th10_stage_saved[2];
static bool th10_stage_saved_valid[2];
static bool th10_in_extra;

static void th10_stage_save() {
	for (int i = 0; i < 2; ++i) {
		th10_stage_saved_valid[i] = false;
		void* stage = *th10_stage_slots[i];
		if (stage == nullptr)
			continue;
		if (th10_stage_saved[i] == nullptr)
			th10_stage_saved[i] = (unsigned char*)malloc(th10_stage_size);
		if (th10_stage_saved[i] == nullptr)
			continue;
		memcpy(th10_stage_saved[i], stage, th10_stage_size);
		th10_stage_saved_valid[i] = true;
	}
}

static void th10_stage_restore() {
	for (int i = 0; i < 2; ++i) {
		void* stage = *th10_stage_slots[i];
		if (!th10_stage_saved_valid[i] || stage == nullptr)
			continue;
		memcpy(stage, th10_stage_saved[i], th10_stage_size);
		th10_stage_saved_valid[i] = false;
	}
}

// FUN_0043C8B0 builds a fade object and puts both of its tasks on the chains. The call from
// FUN_00402850 at 0x402A61 is the one that creates the stage clear fade from the render chain;
// an extra presentation must not create another one, which is what this wrapper does while
// th10_in_extra is set. The callers ignore the return value (the function only returns the new
// object), so nullptr is safe. Everything else goes through to the game's own factory.
static void* (__stdcall* th10_fade_create)(int mode, int duration, int a3, int a4, int a5) =
	(void*(__stdcall*)(int, int, int, int, int))0x0043C8B0;

static void* __stdcall th10_fade_create_extra_guard(int mode, int duration, int a3, int a4, int a5) {
	if (th10_in_extra)
		return nullptr;
	return th10_fade_create(mode, duration, a3, a4, a5);
}

// ---------------------------------------------------------------------------
// Frame interpolation, stage 1: project the bullets forward
// ---------------------------------------------------------------------------
//
// A stage's bullets live in the manager at [0x4776F0], which FUN_00406060 allocates as one
// 0x3E0B54 byte block and FUN_00405E20 fills in: it loads bullet.anm into manager+0x3E0B50, points
// manager+0x10 at the pool that starts at manager+0x60 and holds 2001 bullets of 0x7F0 bytes each,
// and registers the manager's two tasks - the update one (thunk 0x406770) and the draw one (thunk
// 0x4067A0).
//
// The update (FUN_004065C0) clears the six column heads at manager+0x14 and the six tails at
// manager+0x2C, walks the pool, runs every bullet whose state at +0x446 is not 0 through
// FUN_00406240, and threads it onto the list of the column it keeps as a float at +0x460; the link
// to the next bullet of its column is its own +0x44C, and the count of the ones in play is at
// manager+0x5C. The draw (FUN_004066E0, which the draw thunk calls once per column) walks those
// same lists, so they are exactly the bullets that get drawn - that is the list walked here, and it
// is read after the game's own update ran, which is the case by the time a frame is presented.
//
// A bullet moves by its velocity: FUN_00406240 adds the three floats at +0x3C0/+0x3C4/+0x3C8 to
// the position at +0x3B4/+0x3B8/+0x3BC, scaled by the global 0x476F78, once per frame. It moves at
// half of that while its state at +0x446 is 2 or 3 (that half is the float at 0x470B0C), and not at
// all in any other state. The draw follows on its own: FUN_004066E0 copies the position into the
// fields at +0x33C/+0x340/+0x344 (adding the play field's own origin, 224.0f at 0x470B4C and 16.0f
// at 0x470B48) right before it hands the bullet to the queue, so moving the position is enough -
// the same rule as in th11 to th15.

static void** const th10_bullet_manager = (void**)0x004776F0;
static const size_t th10_bullet_lists = 0x14;      // in the manager: the first column's head
static const size_t th10_bullet_columns = 6;       // ...and how many columns there are
static const size_t th10_bullet_next = 0x44C;      // in a bullet: the next one of its column
static const size_t th10_bullet_position = 0x3B4;  // in a bullet: its position, 3 floats
static const size_t th10_bullet_velocity = 0x3C0;  // in a bullet: what it moves by, 3 floats
static const size_t th10_bullet_state = 0x446;     // in a bullet: what it's doing, 16 bits
// The factor the game scales every movement with (1 unless it is slowing everything down)
static float* const th10_time_scale = (float*)0x00476F78;
// How many of the game's updates it runs per rendered frame, minus one
static unsigned char* const th10_frames_per_render = (unsigned char*)0x00491D66;
// The half a bullet in state 2 or 3 moves by (the game multiplies by this one, 0.5)
static float* const th10_half_step = (float*)0x00470B0C;
// The object of the pause screen while it is up, and the flags of it the bullet tasks look at
static void** const th10_pause_object = (void**)0x00477810;
static const size_t th10_pause_flags = 0x58;

// The pool holds 2001 bullets, so the lists of the ones in play can't be longer
static const size_t th10_bullets_saved_max = 2001;
struct Th10SavedBullet {
	void* bullet;
	float position[3];
};
static Th10SavedBullet th10_saved_bullets[th10_bullets_saved_max];
static size_t th10_saved_bullets_count;

// What the last game frame had in play and how much of it was projected forward, for the
// diagnostics dump below
static unsigned int th10_bullets_in_play;
static unsigned int th10_bullets_moved;

// Whether the pause screen has the picture held. A picture the game isn't updating (or isn't
// drawing) must not be moved forward either. 0x406770 and 0x4067A0 are the game's two bullet
// tasks, and this is the same test each of them makes - plus the one the pool walk makes per
// bullet (a held picture leaves the bullets in their lists but doesn't run them).
static bool th10_held() {
	void* pause = *th10_pause_object;
	if (pause == nullptr)
		return false;
	const unsigned int flags = *(unsigned int*)((char*)pause + th10_pause_flags);
	if (((flags | (flags >> 2)) & 1) != 0 || (flags & 4) != 0)
		return true;
	return (flags & 2) != 0 && (flags & 0x400) != 0;
}

// How much of a frame's movement a bullet in this state gets (FUN_00406240)
static float th10_bullet_step_factor(unsigned short state) {
	switch (state) {
	case 1:
		return 1.0f;
	case 2:
	case 3:
		return *th10_half_step;
	default:
		return 0.0f;
	}
}

// Moves every bullet in play forward by t frames and remembers what it changed
static void th10_bullets_advance(float t) {
	th10_saved_bullets_count = 0;
	th10_bullets_in_play = 0;
	th10_bullets_moved = 0;

	void* manager = *th10_bullet_manager;
	if (manager == nullptr || th10_held())
		return;

	unsigned int frames_per_render = *th10_frames_per_render;
	if (frames_per_render > 2) // the game's own setting only goes to 2
		frames_per_render = 2;

	// The factor the game's own update multiplies every movement by, as it is right now - a bullet
	// that isn't moving for the game isn't moved here either
	float scale = *th10_time_scale;
	if (!(scale >= 0.0f && scale <= 2.0f))
		scale = 1.0f;

	const float step = t * (float)(frames_per_render + 1) * scale;

	for (size_t column = 0; column < th10_bullet_columns; ++column) {
		void* bullet = *(void**)((char*)manager + th10_bullet_lists + column * 4);
		for (size_t guard = 0; bullet != nullptr && guard < th10_bullets_saved_max; ++guard) {
			void* next = *(void**)((char*)bullet + th10_bullet_next);

			++th10_bullets_in_play;
			const float factor = th10_bullet_step_factor(*(unsigned short*)((char*)bullet + th10_bullet_state));
			if (factor != 0.0f && th10_saved_bullets_count < th10_bullets_saved_max) {
				float* position = (float*)((char*)bullet + th10_bullet_position);
				const float* velocity = (const float*)((char*)bullet + th10_bullet_velocity);
				Th10SavedBullet& saved = th10_saved_bullets[th10_saved_bullets_count++];
				saved.bullet = bullet;

				const float bullet_step = step * factor;
				for (int axis = 0; axis < 3; ++axis) {
					saved.position[axis] = position[axis];
					position[axis] += velocity[axis] * bullet_step;
				}
				++th10_bullets_moved;
			}

			bullet = next;
		}
	}
}

// Puts the positions of every bullet th10_bullets_advance moved back
static void th10_bullets_undo() {
	for (size_t i = 0; i < th10_saved_bullets_count; ++i) {
		const Th10SavedBullet& saved = th10_saved_bullets[i];
		float* position = (float*)((char*)saved.bullet + th10_bullet_position);
		for (int axis = 0; axis < 3; ++axis)
			position[axis] = saved.position[axis];
	}
	th10_saved_bullets_count = 0;
}

	// ---------------------------------------------------------------------------
	// Frame interpolation, stage 1: the player, its options and its shots, and the sprites they wear
	// ---------------------------------------------------------------------------
	//
	// The player of the stage that is running is at [0x477834] (0x4478 bytes, cleared by
	// FUN_004246C0 once the stage sets it up). Its update (FUN_004250B0, which the stage's own
	// update calls) keeps the position it moves as two ints in 1/128 pixel units at +0x3CC/+0x3D0
	// (clamped to the play field, +/-0x47E0 across and 0xC80 to 0xA8C0 down), adds the two speeds at
	// +0x3F0/+0x3F4 to them, and writes the two floats at +0x3C0/+0x3C4 back out from them (times
	// 1/128, the float at 0x470B00).
	//
	// What the player puts on screen is placed by the update, not by a draw callback: the animation
	// object it wears as a handle at +0x329C is put where it is with FUN_004492F0, and each of its
	// options with FUN_00449350. Those two are the whole of it - FUN_004491C0 is what finds the
	// object a handle names, and the two setters write the drawn position at +0x340/+0x344/+0x348 of
	// it (the second one adds the play field's own origin, 224.0f at 0x470B4C and 16.0f at 0x470B48,
	// which is the offset the bullets carry around as well), moving the objects hanging off it (the
	// node chain at +0x14, unless the flag at +0x18 says not to) along with it. So those sprite
	// objects are what an extra presentation has to move - the same rule th13 and th15 follow, and
	// where th10 differs from th11, whose player's draw copies the position into its sprite every
	// frame and can be moved by its logical position alone.
	//
	// A handle is the sprite object's own first dword, and FUN_004491C0 finds one by walking the two
	// lists the sprite manager keeps at +0x72DAD4 and +0x72DADC (node[0] = the object, node[4] =
	// next), which is written out here rather than called: a slot can keep a stale handle after the
	// game takes its sprite away, and the sprites are looked up again every frame for the reason
	// th12 has to do the same.
	//
	// None of them has a velocity worth reading - the player moves by whatever the input says, the
	// options follow it and whatever they put on screen follows them - so what each of them moves by
	// in a frame is measured, the way th11 does it: every game frame the position is compared with
	// the one of the frame before, and an extra presentation moves the sprite by that difference
	// times the fraction of a frame it stands for, then puts it back.

	static void** const th10_player = (void**)0x00477834;
	static const size_t th10_player_position = 0x3C0;        // 2 floats, in pixels
	static const size_t th10_player_sprite = 0x329C;         // the handle of its own sprite
	// The animation the player object carries itself, drawn at +0x354/+0x358 (FUN_00426360 copies
	// the logical position into it every frame, adding the play field's origin)
	static const size_t th10_player_drawn = 0x354;
	static const size_t th10_player_options = 0x32A0;        // 4 slots of 0x98 bytes
	static const size_t th10_player_option_count = 4;
	static const size_t th10_player_option_size = 0x98;
	static const size_t th10_player_option_position = 0x34;  // 2 ints, in 1/128 pixels
	static const size_t th10_player_option_sprite = 0x68;    // 2 handles
	static const size_t th10_sprite_position = 0x340;        // in a sprite object: 3 floats
	static const size_t th10_sprite_children = 0x14;         // node[0] = child, node[4] = next
	static const size_t th10_sprite_children_off = 0x18;     // non-zero: the children are not moved
	static const size_t th10_sprite_lists[2] = { 0x72DAD4, 0x72DADC };
	// The option positions are kept in 1/128 pixel units
	static const float th10_units_per_pixel = 128.0f;
	static const float th10_max_step = 48.0f;

	// The shots are a pool of the player's own, 128 slots of 0x5C bytes at +0x49C. That pool is what
	// three of the player's functions work on: FUN_00427E90 fills a slot in when the firing pattern
	// (FUN_00428160, walked from the table the stage put at [player+0x45C]+0x110) fires one,
	// FUN_00428280 walks the whole pool every frame (its own slot pointer starts 0x20 into the slot,
	// at +0x4BC, and runs 0x5C at a time), and FUN_00428630 tests the slots against the enemies. A
	// slot keeps:
	//   +0x14/+0x18/+0x1C  the position: 3 floats in pixels, started at the player's own
	//                      +0x3C0/+0x3C4/+0x3C8 or at one of the options' +0x3244/+0x3248 (in 1/128)
	//   +0x40              non-zero while the slot is in use - that is what both of them test, and
	//                      what FUN_00428280 clears when it takes the slot back
	//   +0x44 and +0x48    the two sprite handles the shot is drawn as (FUN_00428280 puts both of
	//                      them where the slot's position is, at +0x340/+0x344/+0x348 = position +
	//                      the play field origin 224.0f/16.0f, so the sprites are what has to move)
	//   +0x58              the firing pattern entry the shot was made from
	// The slot moves itself with the direction and speed it was started with (+0x20/+0x24, +0x2C...
	// the pool updates those itself), so, as in th11, what each one moves by in a frame is measured
	// rather than read.
	static const size_t th10_player_shots = 0x49C;
	static const size_t th10_player_shot_count = 0x80;
	static const size_t th10_player_shot_size = 0x5C;
	static const size_t th10_player_shot_live = 0x40;     // non-zero while the slot is in use
	static const size_t th10_player_shot_position = 0x14; // 2 floats, in pixels
	static const size_t th10_player_shot_sprite = 0x44;   // 2 handles
	// The stage also builds one single shot object (FUN_004056B0, 0x48 bytes, kept at [0x4776EC]),
	// which FUN_00405750 updates - it is what the task FUN_004055C0 registers (callback 0x405840,
	// the object itself as the context) runs - and which FUN_00405860 fills in again from the
	// player's own shot state machine (FUN_00425730) whenever its state at +0x28 is 0. Its position
	// is at +0x30/+0x34 (in pixels, the y one subtracted from every frame) and the sprite it is
	// drawn as is the handle at +0x2C, put where it is with FUN_004492F0 - the same setter, and the
	// same 224.0f/16.0f origin, the pool's sprites are drawn with. It is a shot of the player's too
	// (a single one rather than a pool), so an extra presentation moves its sprite the same way.
	static void** const th10_shot_object = (void**)0x004776EC;
	static const size_t th10_shot_object_state = 0x28;    // 1 while it is in flight
	static const size_t th10_shot_object_sprite = 0x2C;   // 1 handle
	static const size_t th10_shot_object_position = 0x30; // 2 floats, in pixels

	static unsigned int th10_shots_in_flight;     // live slots the pool had, last game frame
	static unsigned int th10_shot_frames;         // game frames of the recorded ones with one live
	static unsigned int th10_shot_object_frames;  // the same, for the single shot object

// What one thing moved by in a frame, kept per thing the player owns. Every one of them is a fixed
// slot of the player object (the player itself, one of the 4 options), so a record never has to be
// recycled: an empty option slot simply has nothing to move.
struct Th10Motion {
	unsigned int frame;
	float last_x, last_y;
	float delta_x, delta_y;
};

static unsigned int th10_motion_frame = 1;

static void th10_measure(Th10Motion& motion, float x, float y, float max_step) {
	// Only a thing that was measured in the frame right before this one has something to compare
	// against - one that just appeared has not moved as far as this can tell
	if (motion.frame != 0 && motion.frame + 1 == th10_motion_frame) {
		const float dx = x - motion.last_x;
		const float dy = y - motion.last_y;
		if (dx > -max_step && dx < max_step && dy > -max_step && dy < max_step) {
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
	motion.frame = th10_motion_frame;
	motion.last_x = x;
	motion.last_y = y;
}

// The sprite object a handle names, or null if it names none that is live right now. The object
// keeps its own address in its first dword and is threaded onto one of the sprite manager's two
// lists, which is what the game's own lookup (FUN_004491C0) walks. This walks them here rather than
// calling it, because its arguments are split between registers and the stack.
static void* th10_sprite_by_handle(unsigned int handle) {
	if (handle == 0)
		return nullptr;
	void* manager = *(void**)th10_sprite_manager;
	if (manager == nullptr)
		return nullptr;
	for (size_t list = 0; list < 2; ++list) {
		void* node = *(void**)((char*)manager + th10_sprite_lists[list]);
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
struct Th10Shifted {
	float* position;
	float saved[3];
	float moved_x, moved_y;   // what was added to it, to see later whether it stayed
};

static const size_t th10_shifted_max = 4096;
static Th10Shifted th10_shifted[th10_shifted_max];
static size_t th10_shifted_count;
static unsigned int th10_shifts_lost;

// Moves one position and remembers what it changed
static void th10_shift_position(float* position, const Th10Motion& motion, float t) {
	if (position == nullptr || th10_shifted_count >= th10_shifted_max)
		return;
	Th10Shifted& shifted = th10_shifted[th10_shifted_count++];
	shifted.position = position;
	for (int axis = 0; axis < 3; ++axis)
		shifted.saved[axis] = position[axis];
	position[0] += motion.delta_x * t;
	position[1] += motion.delta_y * t;
	shifted.moved_x = motion.delta_x * t;
	shifted.moved_y = motion.delta_y * t;
}

// The same, for the drawn position of one sprite object
static void th10_shift_object(void* object, const Th10Motion& motion, float t) {
	if (object == nullptr)
		return;
	th10_shift_position((float*)((char*)object + th10_sprite_position), motion, t);
}

// The same, for a sprite the game hands out: the objects hanging off it (the node chain at +0x14)
// are given the same position unless the flag at +0x18 says not to (FUN_004492F0 and FUN_00449350
// both do that), so they move with it here as well.
static void th10_shift_sprite(void* sprite, const Th10Motion& motion, float t) {
	th10_shift_object(sprite, motion, t);
	if (*(const int*)((char*)sprite + th10_sprite_children_off) != 0)
		return;
	void* node = *(void**)((char*)sprite + th10_sprite_children);
	for (size_t guard = 0; node != nullptr && guard < 16; ++guard) {
		th10_shift_object(*(void**)node, motion, t);
		node = *(void**)((char*)node + 4);
	}
}

static void th10_undo_shifts() {
	// Something in the render walk can put a position back where it was before the walk drew it
	// (an animation callback that owns the position, for example), which would leave the extra
	// presentation looking exactly like the game's own. Counting those is what says whether the
	// field being moved is the one the renderer ends up reading.
	th10_shifts_lost = 0;
	for (size_t i = 0; i < th10_shifted_count; ++i) {
		const Th10Shifted& shifted = th10_shifted[i];
		if (shifted.position[0] != shifted.saved[0] + shifted.moved_x ||
				shifted.position[1] != shifted.saved[1] + shifted.moved_y)
			++th10_shifts_lost;
		for (int axis = 0; axis < 3; ++axis)
			shifted.position[axis] = shifted.saved[axis];
	}
	th10_shifted_count = 0;
}

// The player itself: a motion record of its own, and nothing else to keep
static Th10Motion th10_body_motion;

// The options: what each of the 4 slots moved by, and the two sprites it wears
static Th10Motion th10_option_motions[th10_player_option_count];
static void* th10_option_sprites[th10_player_option_count][2];

// The shots: what each of the 128 slots of the player's pool moved by, the two sprites it wears, and
// the handle of the first one the frame before, which says whether the slot was fired again since
static Th10Motion th10_shot_motions[th10_player_shot_count];
static void* th10_shot_sprites[th10_player_shot_count][2];
static unsigned int th10_shot_handles[th10_player_shot_count];

// The one single shot object, and the sprite it wears this frame
static Th10Motion th10_shot_object_motion;
static void* th10_shot_object_sprite_object;

// What the last game frame had on screen and how much of it was projected forward, for the
// diagnostics dump below
static unsigned int th10_sprites_untracked;
static unsigned int th10_body_sprites;
static unsigned int th10_option_sprites_moved;
static unsigned int th10_shot_sprites_moved;
static float th10_shot_movement;
static float th10_player_movement;

// Collects what the player owns this frame and works out what it moved by since the frame before.
// This runs once per game frame, from the game's own present (see th10_present_hook).
static void th10_player_measure() {
	++th10_motion_frame;
	th10_sprites_untracked = 0;
	th10_body_sprites = 0;
	th10_option_sprites_moved = 0;
	th10_shot_sprites_moved = 0;
	th10_shots_in_flight = 0;

	// The shots, from the pool the player keeps them in (see th10_player_shots above)
	{
		void* player = *th10_player;
		for (unsigned int i = 0; i < th10_player_shot_count; ++i) {
			const char* slot = player == nullptr
				? nullptr : (const char*)player + th10_player_shots + i * th10_player_shot_size;
			if (slot == nullptr || *(const int*)(slot + th10_player_shot_live) == 0) {
				th10_shot_sprites[i][0] = nullptr;
				th10_shot_sprites[i][1] = nullptr;
				th10_shot_motions[i].frame = 0;
				th10_shot_handles[i] = 0;
				continue;
			}
			++th10_shots_in_flight;
			// A slot the pattern has fired into since the frame before is a new shot that happens to
			// have taken the slot over: it has no movement to project yet
			const unsigned int handle = *(const unsigned int*)(slot + th10_player_shot_sprite);
			const bool refired = handle != 0 && handle != th10_shot_handles[i];
			th10_shot_handles[i] = handle;
			const float* position = (const float*)(slot + th10_player_shot_position);
			th10_measure(th10_shot_motions[i], position[0], position[1], th10_max_step);
			if (refired) {
				th10_shot_motions[i].delta_x = 0.0f;
				th10_shot_motions[i].delta_y = 0.0f;
			}
			{
				const float dx = th10_shot_motions[i].delta_x < 0.0f
					? -th10_shot_motions[i].delta_x : th10_shot_motions[i].delta_x;
				const float dy = th10_shot_motions[i].delta_y < 0.0f
					? -th10_shot_motions[i].delta_y : th10_shot_motions[i].delta_y;
				const float magnitude = dx > dy ? dx : dy;
				if (magnitude > th10_shot_movement)
					th10_shot_movement = magnitude;
			}
			for (int sprite = 0; sprite < 2; ++sprite) {
				const unsigned int sprite_handle = *(const unsigned int*)(
					slot + th10_player_shot_sprite + sprite * 4);
				th10_shot_sprites[i][sprite] = th10_sprite_by_handle(sprite_handle);
				if (sprite_handle != 0 && th10_shot_sprites[i][sprite] == nullptr)
					++th10_sprites_untracked;
			}
		}
		if (th10_shots_in_flight != 0)
			++th10_shot_frames;
	}

	// The single shot object, if the stage has one and it is in flight (see th10_shot_object above)
	{
		void* shot = *th10_shot_object;
		if (shot != nullptr && *(const int*)((const char*)shot + th10_shot_object_state) == 1) {
			++th10_shot_object_frames;
			const float* position = (const float*)((const char*)shot + th10_shot_object_position);
			th10_measure(th10_shot_object_motion, position[0], position[1], th10_max_step);
			const unsigned int handle =
				*(const unsigned int*)((const char*)shot + th10_shot_object_sprite);
			th10_shot_object_sprite_object = th10_sprite_by_handle(handle);
			if (handle != 0 && th10_shot_object_sprite_object == nullptr)
				++th10_sprites_untracked;
		} else {
			th10_shot_object_sprite_object = nullptr;
			th10_shot_object_motion.frame = 0;
		}
	}

	void* player = *th10_player;
	if (player == nullptr)
		return;

	for (unsigned int i = 0; i < th10_player_option_count; ++i) {
		const char* slot = (const char*)player + th10_player_options + i * th10_player_option_size;
		const int* position = (const int*)(slot + th10_player_option_position);
		if (*(const int*)slot == 0) {
			th10_option_sprites[i][0] = nullptr;
			th10_option_sprites[i][1] = nullptr;
			th10_option_motions[i].frame = 0;
			continue;
		}
		th10_measure(th10_option_motions[i],
			(float)position[0] / th10_units_per_pixel, (float)position[1] / th10_units_per_pixel,
			th10_max_step);
		for (int sprite = 0; sprite < 2; ++sprite) {
			const unsigned int handle =
				*(const unsigned int*)(slot + th10_player_option_sprite + sprite * 4);
			th10_option_sprites[i][sprite] = th10_sprite_by_handle(handle);
			if (handle != 0 && th10_option_sprites[i][sprite] == nullptr)
				++th10_sprites_untracked;
		}
	}

	// The player itself: its own position is what the update moves, and it is that position the
	// sprite it wears is put at, so that is what is measured
	{
		const float* position = (const float*)((const char*)player + th10_player_position);
		th10_measure(th10_body_motion, position[0], position[1], th10_max_step);
		const float x = th10_body_motion.delta_x < 0.0f ? -th10_body_motion.delta_x : th10_body_motion.delta_x;
		const float y = th10_body_motion.delta_y < 0.0f ? -th10_body_motion.delta_y : th10_body_motion.delta_y;
		const float magnitude = x > y ? x : y;
		if (magnitude > th10_player_movement)
			th10_player_movement = magnitude;
	}
}

// Moves the player and everything it put on screen t frames ahead of the state the game is in, and
// remembers what it changed
static void th10_player_advance(float t) {
	th10_body_sprites = 0;
	th10_option_sprites_moved = 0;
	th10_shot_sprites_moved = 0;
	th10_shifted_count = 0;

	if (th10_held())
		return;

	void* player = *th10_player;
	if (player == nullptr)
		return;

	if (Config::InterpolatePlayerBody) {
		if (th10_motion_frame == th10_body_motion.frame) {
			// The render walk copies this position into the animation the player object itself
			// carries (drawn at +0x354/+0x358), so moving it here covers the body; the sprite the
			// player wears as a handle is placed by the update instead (FUN_004492F0), so it moves
			// with it, the way th13's notes ended up covering both.
			th10_shift_position((float*)((char*)player + th10_player_position), th10_body_motion, t);
			th10_shift_position((float*)((char*)player + th10_player_drawn), th10_body_motion, t);
			++th10_body_sprites;
			const unsigned int handle = *(const unsigned int*)((char*)player + th10_player_sprite);
			void* sprite = th10_sprite_by_handle(handle);
			if (sprite != nullptr) {
				th10_shift_sprite(sprite, th10_body_motion, t);
			} else if (handle != 0) {
				++th10_sprites_untracked;
			}
		}

		for (unsigned int i = 0; i < th10_player_option_count; ++i) {
			if (th10_motion_frame != th10_option_motions[i].frame)
				continue;
			for (int sprite = 0; sprite < 2; ++sprite) {
				if (th10_option_sprites[i][sprite] == nullptr)
					continue;
				th10_shift_sprite(th10_option_sprites[i][sprite], th10_option_motions[i], t);
				++th10_option_sprites_moved;
			}
		}
	}

	if (Config::InterpolatePlayerShots) {
		// The shots: a slot that was measured this frame still has the sprites it was drawn as,
		// and those are what moves
		for (unsigned int i = 0; i < th10_player_shot_count; ++i) {
			if (th10_motion_frame != th10_shot_motions[i].frame)
				continue;
			for (int sprite = 0; sprite < 2; ++sprite) {
				if (th10_shot_sprites[i][sprite] == nullptr)
					continue;
				th10_shift_sprite(th10_shot_sprites[i][sprite], th10_shot_motions[i], t);
				++th10_shot_sprites_moved;
			}
		}

		// The single shot object: the same, for the one sprite it wears
		if (th10_motion_frame == th10_shot_object_motion.frame &&
			th10_shot_object_sprite_object != nullptr) {
			th10_shift_sprite(th10_shot_object_sprite_object, th10_shot_object_motion, t);
			++th10_shot_sprites_moved;
		}
	}
}

// ---------------------------------------------------------------------------
// Frame interpolation, stage 1: the straight lasers
// ---------------------------------------------------------------------------
//
// The lasers of a stage live in a manager of their own, [0x47781C], which FUN_0041C290 builds as
// one 0x45C byte block (the node the game works from sits at +0x10 of it) and FUN_0041C120 fills in
// when a stage starts - that is where bullet.anm goes, into manager+0x458, and where the manager's
// two tasks are registered: the update walk (0x41C480 -> FUN_0041C330) and the draw walk (0x41C4E0).
// The draw walk starts from the root node's next at [manager+0x18] and follows each object's own
// next at +0x08, skipping every one whose state at +0x0C is 1 - the same test the update's own
// removal makes. The objects are of the two classes FUN_0041C5B0 and FUN_0041C680 build (0xD58 and
// 0xD74 bytes, vtables 0x46DA60 and 0x46DA10), and both of them are lasers: their own draw
// (FUN_0041D7C0 and the one at 0x41EA40) copies the position at +0x24/+0x28/+0x2C, with the play
// field origin 224.0f at 0x470B4C and 16.0f at 0x470B48, and the angle at +0x3C into the two
// animation objects each of them embeds (+0x600 and +0x9AC, +0x61C and +0x9C8) and queues those with
// FUN_004451C0. The draw runs on the render walk, which is what an extra presentation runs again, so
// the position and the angle are what the beam on screen is drawn from.
//
// What each of the two classes moves by is in its own places, read out of its own update
// (FUN_0041D3D0 and FUN_0041E700), where each of them multiplies one of its own fields by the
// global scale at 0x476F78 and adds it to the field that field belongs to:
//
//   the class at vtable 0x46DA60  velocity +0x30/+0x34/+0x38, the length grows by the rate at +0x48
//                                 up to the limit at +0x434, and it does not turn - its update
//                                 never writes the angle.
//   the class at vtable 0x46DA10  velocity +0x430/+0x434/+0x438, the angle turns by the speed at
//                                 +0x440, and the length grows by the same +0x48 up to +0x444.
//
// Both keep the position at +0x24/+0x28/+0x2C, the angle at +0x3C and the length at +0x40, which is
// the same three fields th11 projects its lasers from. The length is carried along even though the
// draw reads nothing of the sort (the drawn size of a beam is a sprite field the game's own update
// computes from the length once a frame, so the beam still grows in whole frames rather than
// smoothly): it is what the game's own update moves, and carrying it keeps the two in step.

// One class of straight laser: where it keeps what it moves by, and what it grows towards
struct Th10LaserLayout {
	const void* vtable;
	size_t velocity;      // 3 floats
	size_t angle_speed;   // 0 for a class that doesn't turn
	size_t length_speed;
	size_t length_limit;
};

static void** const th10_laser_manager = (void**)0x0047781C;
static const size_t th10_laser_list = 0x18;      // in the manager: the first laser of the list
static const size_t th10_laser_next = 0x08;      // in a laser: the next one
static const size_t th10_laser_state = 0x0C;     // in a laser: 1 while it isn't drawn yet
static const size_t th10_laser_position = 0x24;  // where it starts, 3 floats
static const size_t th10_laser_angle = 0x3C;     // the direction it points in, radians
static const size_t th10_laser_length = 0x40;    // how far it reaches

static const size_t th10_lasers_max = 0x100 + 1; // the manager's own limit, plus one for safety
static const Th10LaserLayout th10_laser_layouts[] = {
	// The class that doesn't turn on its own, and whose length stops at +0x434
	{ (const void*)0x0046DA60, 0x30,  0,     0x48, 0x434 },
	// The class that turns by +0x440, and whose length stops at +0x444
	{ (const void*)0x0046DA10, 0x430, 0x440, 0x48, 0x444 },
};

// The layout of the class this laser is, or null if it isn't one of the straight ones
static const Th10LaserLayout* th10_laser_layout(const void* laser) {
	const void* vtable = *(const void* const*)laser;
	for (size_t i = 0; i < sizeof(th10_laser_layouts) / sizeof(th10_laser_layouts[0]); ++i) {
		if (vtable == th10_laser_layouts[i].vtable)
			return &th10_laser_layouts[i];
	}
	return nullptr;
}

struct Th10SavedLaser {
	void* laser;
	float position[3];
	float angle, length;
};

static Th10SavedLaser th10_saved_lasers[th10_lasers_max];
static size_t th10_saved_lasers_count;
static unsigned int th10_lasers_in_play;
static unsigned int th10_lasers_projected;

// Moves the straight lasers t frames ahead of the state the game is in, and remembers what it
// changed. Each laser is put where the velocity it carries right now would have it t frames from
// here: the game's own update adds velocity * the global scale to the position once a frame, so
// this is that same step, taken for the fraction of a frame this presentation stands for.
static void th10_lasers_advance(float t) {
	th10_saved_lasers_count = 0;
	th10_lasers_in_play = 0;
	th10_lasers_projected = 0;

	void* manager = *th10_laser_manager;
	if (manager == nullptr || th10_held())
		return;

	unsigned int frames_per_render = *th10_frames_per_render;
	if (frames_per_render > 2) // the game's own setting only goes to 2
		frames_per_render = 2;

	float scale = *th10_time_scale;
	if (!(scale >= 0.0f && scale <= 2.0f))
		scale = 1.0f;

	const float step = t * (float)(frames_per_render + 1) * scale;

	void* laser = *(void**)((char*)manager + th10_laser_list);
	// The list is the game's own and this runs where nothing can change it; the bound is only so a
	// list that got broken some other way can't run away with the frame
	for (size_t guard = 0; laser != nullptr && guard < th10_lasers_max * 4; ++guard) {
		void* next = *(void**)((char*)laser + th10_laser_next);
		const Th10LaserLayout* layout = th10_laser_layout(laser);
		// One whose state is 1 isn't drawn yet: the draw walk skips those
		if (layout != nullptr && *(const int*)((char*)laser + th10_laser_state) != 1) {
			++th10_lasers_in_play;
			if (th10_saved_lasers_count < th10_lasers_max) {
				Th10SavedLaser& saved = th10_saved_lasers[th10_saved_lasers_count++];
				saved.laser = laser;

				float* position = (float*)((char*)laser + th10_laser_position);
				float* angle = (float*)((char*)laser + th10_laser_angle);
				float* length = (float*)((char*)laser + th10_laser_length);
				for (int axis = 0; axis < 3; ++axis)
					saved.position[axis] = position[axis];
				saved.angle = *angle;
				saved.length = *length;

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
				++th10_lasers_projected;
			}
		}
		laser = next;
	}
}

// Puts the position, the angle and the length of every laser th10_lasers_advance moved back
static void th10_lasers_undo() {
	for (size_t i = 0; i < th10_saved_lasers_count; ++i) {
		const Th10SavedLaser& saved = th10_saved_lasers[i];
		float* position = (float*)((char*)saved.laser + th10_laser_position);
		for (int axis = 0; axis < 3; ++axis)
			position[axis] = saved.position[axis];
		*(float*)((char*)saved.laser + th10_laser_angle) = saved.angle;
		*(float*)((char*)saved.laser + th10_laser_length) = saved.length;
	}
	th10_saved_lasers_count = 0;
}

// Puts the sprite queue back to empty, the way the frame function inlines it (0x43949C)
static void th10_reset_sprite_queue() {
	void* manager = *(void**)th10_sprite_manager;
	if (manager == nullptr)
		return;
	DWORD base = (DWORD)manager + 0x3ADACC;
	*(DWORD*)((char*)manager + 0x3ADAC8) = 0;
	*(DWORD*)((char*)manager + 0x72DACC) = base;
	*(DWORD*)((char*)manager + 0x72DAD0) = base;
}

// Remembers what the scene is drawn with, then lets the game's own render run exactly as it always
// did (see th10_extra_presentation for what it's for)
// The screen shake: GameManager's shake update (0x43C5CA) rolls the game's own random number
// generator, takes it modulo 3 and writes 0, +offset or -offset into the Float2 at 0x491E64 (x) and
// 0x491E68 (y); the drawing paths read it at 0x40288D and 0x402CD9, and the pass clears it at
// 0x4200DC and 0x448747. An extra pass that starts after that clear draws the frame unshaken, so the
// value the game's own pass started with is put back around every extra pass.
static ExtraFloatState th10_shake = { (float*)0x00491E64, 2 };

static int __stdcall th10_render_hook(void* scene) {
	IDirect3DDevice9* device = th10_device();
	if (device != nullptr) {
		device->GetViewport(&th10_scene_viewport);
		th10_scene_transforms = SUCCEEDED(device->GetTransform(D3DTS_VIEW, &th10_scene_view)) &&
			SUCCEEDED(device->GetTransform(D3DTS_PROJECTION, &th10_scene_projection));
	}
	th10_scene_context = *(DWORD*)th10_render_context;
	void* manager = *(void**)th10_sprite_manager;
	if (manager != nullptr) {
		th10_scene_sprite_offset[0] = *(DWORD*)((char*)manager + th10_sprite_screen_offset);
		th10_scene_sprite_offset[1] = *(DWORD*)((char*)manager + th10_sprite_screen_offset + 4);
	}

	ToolState::WatchFrameReady(th10_tool_frame_address, th10_tool_draw_address,
		th10_tool_plain_byte, true);

	// What the game's own run of the walk finds the text lists at, for the extra presentations to
	// put back (see th10_text_records)
	th10_text_records_save();

	extra_float_state_save(&th10_shake);

	return th10_render_objects(scene);
}

// Diagnostics: press U to write the presentation timing of the last frames to oilp_diag.txt
static const unsigned int th10_diag_frames_max = 300;
static const unsigned int th10_diag_presents_max = 8;
struct Th10DiagFrame {
	LARGE_INTEGER base;
	unsigned int presents;
	unsigned int updates;      // how many times the game's own update walk ran in this frame
	int stage_timer;           // the stage's own frame counter at the end of it
	float game_rate;           // the frame rate the game itself worked out (fps.cpp), and its count
	int game_frames;
	LARGE_INTEGER present_time[th10_diag_presents_max];
};
static Th10DiagFrame th10_diag_frames[th10_diag_frames_max];
static unsigned int th10_diag_count;
static bool th10_diag_key_down;

static LARGE_INTEGER th10_perf_freq;

static void th10_diag_dump() {
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

	unsigned int count = th10_diag_count < th10_diag_frames_max ? th10_diag_count : th10_diag_frames_max;
	unsigned int start = th10_diag_count < th10_diag_frames_max ? 0 : th10_diag_count % th10_diag_frames_max;

	fprintf(file, "oilp frame interpolation diagnostics\n");
	fprintf(file, "logic frame rate: %.2f, configured presentation rate: %.2f\n",
		Limiter::game_fps, Limiter::PresentRate());
	fprintf(file, "frames recorded: %u\n", count);
	fprintf(file, "bullets: %u in play, %u projected forward (last game frame)\n",
		th10_bullets_in_play, th10_bullets_moved);
	fprintf(file, "lasers: %u in play, %u projected forward (last game frame)\n",
		th10_lasers_in_play, th10_lasers_projected);
	fprintf(file, "player: %u body, %u option and %u shot sprite(s) projected (last game frame, %u shot(s) in flight)\n",
		th10_body_sprites, th10_option_sprites_moved, th10_shot_sprites_moved, th10_shots_in_flight);
	fprintf(file, "player: %u sprite(s) this frame had no movement record (a handle the game had just"
		" taken the sprite away from; costs that sprite one frame)\n",
		th10_sprites_untracked);
	fprintf(file, "player: %u moved position(s) were written back during the walk (0: the renderer reads them)\n",
		th10_shifts_lost);
	fprintf(file, "player movement: up to %.2f px per frame since the last dump\n", th10_player_movement);
	th10_player_movement = 0.0f;
	fprintf(file, "shot movement: up to %.2f px per frame since the last dump\n", th10_shot_movement);
	th10_shot_movement = 0.0f;
	fprintf(file, "shots: %u game frame(s) since the last dump had one of the player's pool in flight"
		" (%u had the single shot object)\n", th10_shot_frames, th10_shot_object_frames);
	th10_shot_frames = 0;
	th10_shot_object_frames = 0;

	double presents = 0.0;
	double first = 0.0;
	double last = 0.0;
	unsigned int updates_min = 0, updates_max = 0;
	int stage_timer_first = 0, stage_timer_last = 0;
	for (unsigned int i = 0; i < count; ++i) {
		const Th10DiagFrame& frame = th10_diag_frames[(start + i) % th10_diag_frames_max];
		fprintf(file, "frame %4u: %u present(s) %u update(s)", i, frame.presents, frame.updates);
		for (unsigned int p = 0; p < frame.presents && p < th10_diag_presents_max; ++p)
			fprintf(file, " [%u] %.2fms", p,
				(double)(frame.present_time[p].QuadPart - frame.base.QuadPart) * 1000.0 / (double)th10_perf_freq.QuadPart);
		fprintf(file, " stage timer %d", frame.stage_timer);
		fprintf(file, " game rate %.1f (%d counted)", frame.game_rate, frame.game_frames);
		fprintf(file, "\n");
		if (i == 0) {
			updates_min = updates_max = frame.updates;
			stage_timer_first = frame.stage_timer;
		}
		if (frame.updates < updates_min)
			updates_min = frame.updates;
		if (frame.updates > updates_max)
			updates_max = frame.updates;
		stage_timer_last = frame.stage_timer;
		presents += frame.presents;
		if (i == 0)
			first = (double)frame.base.QuadPart;
		last = (double)frame.base.QuadPart;
	}
	fprintf(file, "updates per frame: %u to %u\n", updates_min, updates_max);
	fprintf(file, "stage timer: %d to %d over %u frame(s)\n",
		stage_timer_first, stage_timer_last, count);
	if (last > first)
		fprintf(file, "average: %.3f presents per frame, %.1f presentations per second\n",
			presents / (double)count, presents * (double)th10_perf_freq.QuadPart / (last - first));

	fclose(file);
	printf("Wrote oilp_diag.txt\n");
}

// One extra presentation of the frame: the same render block the game runs, once more, so the state
// the game is in is drawn and presented again. The tool's frame is readied again, so the object
// render below draws its overlay into this presentation the same way it does in the game's own one.
static void th10_extra_presentation(IDirect3DDevice9* device, float t) {
	(void)t; // stage 1 is what moves anything; this draws the state the game is already in

	device->BeginScene();
	if (th10_scene_transforms) {
		device->SetTransform(D3DTS_VIEW, &th10_scene_view);
		device->SetTransform(D3DTS_PROJECTION, &th10_scene_projection);
	}
	if (th10_scene_viewport.Width != 0)
		device->SetViewport(&th10_scene_viewport);
	// The 2D context the game's own frame rendered with, and the screen placement it left in the
	// sprite queue (see th10_scene_viewport)
	*(DWORD*)th10_render_context = th10_scene_context;
	{
		void* manager = *(void**)th10_sprite_manager;
		if (manager != nullptr) {
			*(DWORD*)((char*)manager + th10_sprite_screen_offset) = th10_scene_sprite_offset[0];
			*(DWORD*)((char*)manager + th10_sprite_screen_offset + 4) = th10_scene_sprite_offset[1];
		}
	}
	th10_reset_sprite_queue();
	*(DWORD*)0x00492378 = 0xFF;
	th10_prepare_render();
	ToolState::ReadyToDraw();
	// The text lists go back to how the game's own run of the walk found them, so this presentation
	// draws the same text that one drew (see th10_text_records above)
	th10_text_records_restore();
	// Same for the game's own frame rate accounting: this presentation shows the rate the game's
	// own frame showed and isn't counted as a frame of its own (see th10_fps above)
	th10_fps_restore(true);
	// Everything is drawn where it will be t frames from now; the render bakes those positions into
	// what it draws, so they can all go back right after it
	if (Config::InterpolateBulletsAndLasers) {
		th10_bullets_advance(t);
	}
	th10_player_advance(t);
	if (Config::InterpolateBulletsAndLasers) {
		th10_lasers_advance(t);
	}
	// Nothing to render into an empty scene, and the walk itself would read the object it is handed
	void* scene = *th10_scene;
	if (scene != nullptr) {
		// The stage's draw callbacks advance its transition (see th10_stage_save); put the object
		// back afterwards so an extra pass cannot move the transition a second or third time
		th10_stage_save();
		th10_in_extra = true;
		// Put the shake back for this pass (see the comment above)
		extra_float_state_begin(&th10_shake);
		th10_render_objects(scene);
		extra_float_state_end(&th10_shake);
		th10_in_extra = false;
		th10_stage_restore();
	}
	th10_lasers_undo();
	th10_undo_shifts();
	th10_bullets_undo();
	// The walk just ran the game's rate task, so what it made of that has to go back as well
	th10_fps_restore(false);
	th10_draw_sprite_queue();
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
static HRESULT th10_present_hook() {
	IDirect3DDevice9* device = th10_device();
	if (device == nullptr)
		return E_FAIL;

	static bool freq_queried = false;
	if (!freq_queried) {
		freq_queried = true;
		QueryPerformanceFrequency(&th10_perf_freq);
	}

	LARGE_INTEGER base;
	QueryPerformanceCounter(&base);

	// The game's own update has run and it is about to put that frame on screen: this is the one
	// point in the frame where the movement of the player and its options can be measured
	th10_player_measure();

	HRESULT result = device->Present(nullptr, nullptr, nullptr, nullptr);
	overlay_mark_presentation();
	Limiter::MarkFrameWorkEnd();
	ToolState::WatchFrameDrawn();

	// What the game's own frame left its rate accounting at, for the extra presentations to put back
	// (see th10_fps); taken here because this is the point the game's own frame is over
	th10_fps_save();

	unsigned int presents = 1;
	LARGE_INTEGER present_time[th10_diag_presents_max];
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

				if (presents < th10_diag_presents_max)
					QueryPerformanceCounter(&present_time[presents]);
				++presents;

				// Where this presentation sits between the game's own one and the next one:
				// the frames of one game frame are spread evenly over it, so the k-th of
				// extras + 1 of them draws the state k/(extras + 1) frames along
				th10_extra_presentation(device, (float)k / (float)(extras + 1));
			}
		}
	}

	{
		Th10DiagFrame& frame = th10_diag_frames[th10_diag_count % th10_diag_frames_max];
		frame.base = base;
		frame.presents = presents;
		frame.updates = th10_frame_updates;
		th10_frame_updates = 0;
		void* stage = *th10_stage;
		frame.stage_timer = stage != nullptr
			? *(int*)((char*)stage + th10_stage_timer) : 0;
		void* fps = *th10_fps;
		frame.game_rate = fps != nullptr ? *(float*)((char*)fps + 0x34) : 0.0f;
		frame.game_frames = fps != nullptr ? *(int*)((char*)fps + 0x20) : 0;
		for (unsigned int p = 0; p < th10_diag_presents_max; ++p)
			frame.present_time[p] = p < presents ? present_time[p] : base;
		++th10_diag_count;
	}

	if (GetAsyncKeyState('U') & 0x8000) {
		if (!th10_diag_key_down) {
			th10_diag_key_down = true;
			th10_diag_dump();
		}
	} else {
		th10_diag_key_down = false;
	}

	return result;
}

// The present the patch takes over is split in the middle of its own argument setup: the game
// pushes three of the four arguments (0x4391FD, 0x4391FF, 0x439201), then loads the device
// (0x43920A, where the patch puts its call), then pushes the last argument and the `this` pointer
// and calls Present (0x439211 to 0x439216), which is __stdcall and takes all five dwords off the
// stack itself (its `ret 0x14`). The patch jumps over everything from 0x43920F on, so those first
// three pushes are still on the stack when the hook is done; the `ret 12` here is what takes them
// off in place of the present the game would have called, so the site's landing point (0x439217)
// and the stack cookie check at the end of the function see the stack they expect. Leaving them on
// there costs the game its own cookie and it reports that as a buffer overrun.
//
// (The three present sites of th11 and the later games don't need this: their whole argument setup
// is inside the skipped part, so it cancels itself out.)
static void __declspec(naked) th10_present_hook_keeping_arguments() {
	__asm {
		call th10_present_hook
		ret 0xC
	}
}

// Jumps over the present the game sets up after its present call site, so the code after it runs
// with the result the hook left in eax.
static void th10_skip_present_setup(DWORD address, size_t size) {
	BYTE patch[32];
	patch[0] = 0xE9;
	*(DWORD*)(patch + 1) = (address + size) - (address + 5);
	for (size_t i = 5; i < size; ++i)
		patch[i] = 0x90;
	patch_bytes(address, patch, size);
}

void th10_install_patches() {
	{
		// Skip the original frame limiter
		BYTE patch[] = { 0xEB };
		patch_bytes(0x004393C5, patch, sizeof(patch));
	}
	{
		// Don't sleep in the main loop
		BYTE patch[] = { 0x90, 0x90, 0x90, 0x90, 0x90, 0x90 };
		patch_bytes(0x004393B7, patch, sizeof(patch));
	}
	{
		// Hook window update
		patch_call(0x00438D31, th10_window_update_hook);
	}
	{
		// th10 keeps a frame rate of its own and acts on it: once the rate it works out goes above
		// the threshold it has at [0x470BB8] it treats the frame as having run too fast, and after
		// two of those in a row it re-syncs its own clock from the performance counter - after four
		// it clears that counter's base as well, which is what makes the rate it reports read as 0
		// for a while (its own source says so: see fps.cpp, the task FUN_004134B0 runs).
		//
		// Running the game at its own 60 frames a second never reaches that threshold, but raising
		// the frame rate does, right away - the rate is that much higher than 60 by design, and the
		// limiter is what is really pacing the game then. That leaves this protection nothing to act
		// on and only the re-syncing it does on the way, so it goes: the jump here is the same one
		// thprac's own th10 module makes (0x413516, `EB 50`) when it is the one running the game.
		BYTE patch[] = { 0xEB, 0x50 };
		patch_bytes(0x00413516, patch, sizeof(patch));
	}
	if (Config::D3D9Ex) {
		// Redirect Direct3DCreate9 call
		// IAT is being hooked, but thcrap also hooks Direct3DCreate9 in the same place
		// This should force our Direct3DCreate9 hook to be loaded no matter what
		patch_call(0x00438BC3, Direct3DCreate9_hook);
	}
	if (Config::Interpolation || Config::InterpolationFPS) {
		// Take over the call into the game's own update walk as well, only to count how many times
		// it ran for a frame (see th10_update_hook); the walk itself runs exactly as it always did
		BYTE* update_call = (BYTE*)0x0043942F;
		if (update_call[0] == 0xE8) {
			patch_call(0x0043942F, (void*)th10_update_hook);
		} else {
			printf("th10 update call at 0x43942F doesn't look like the known one, skipping!\n");
		}

		// Take over the object render, only to remember what the scene is drawn with (see
		// th10_render_hook); the render itself runs exactly the way it always did
		BYTE* render_call = (BYTE*)0x004394D8;
		unsigned int render_patched = 0;
		if (render_call[0] == 0xE8) {
			patch_call(0x004394D8, (void*)th10_render_hook);
			++render_patched;
		} else {
			printf("th10 render call at 0x4394D8 doesn't look like the known one, skipping!\n");
		}

		// The stage's draw callback FUN_00402850 creates the stage clear fade at 0x402A61; an
		// extra presentation must skip that (see th10_fade_create_extra_guard)
		BYTE* fade_call = (BYTE*)0x00402A61;
		if (fade_call[0] == 0xE8 &&
			(DWORD)0x00402A66 + *(DWORD*)(fade_call + 1) == 0x0043C8B0) {
			patch_call(0x00402A61, (void*)th10_fade_create_extra_guard);
		} else {
			printf("th10 fade creation call at 0x402A61 doesn't look like the known one, skipping!\n");
		}

		// Take over the present inside the frame function's own present helper (FUN_004391F0).
		// The site starts by loading the device (5 bytes, exactly what a hooked call needs) and then
		// pushes its arguments and calls Present over the 8 bytes below (0x43920F is `mov ecx,[eax]`,
		// 0x439211 `push 0`, 0x439213 `push eax` and 0x439214 `call [ecx+44h]`), which are skipped
		// over so 0x439217 (`test eax,eax`) runs with the result the hook left in eax. Three of the
		// four arguments were pushed before the site, so the hook returns through the trampoline that
		// takes them back off (see th10_present_hook_keeping_arguments).
		const size_t present_setup = 8;
		BYTE* present_site = (BYTE*)0x0043920A;
		unsigned int present_patched = 0;
		if (present_site[0] == 0xA1 && *(DWORD*)(present_site + 1) == 0x00491C30) {
			// The setup has to end exactly on the instruction after the call, or the code after it
			// runs from the middle of one and walks the stack off by whatever it decodes to
			if (present_site[5 + present_setup] == 0x85) {
				patch_call(0x0043920A, (void*)th10_present_hook_keeping_arguments);
				th10_skip_present_setup(0x0043920A + 5, present_setup);
				++present_patched;
			} else {
				printf("th10 present call at 0x43920A isn't followed by the code expected after it (0x%02x), skipping!\n",
					present_site[5 + present_setup]);
			}
		} else {
			printf("th10 present call at 0x43920A doesn't look like the known one, skipping!\n");
		}
		printf("Frame interpolation: %u render call(s) and %u present call(s) taken over (setting %d)\n",
			render_patched, present_patched,
			Config::InterpolationFPS ? (int)Config::InterpolationFPS : Config::Interpolation);

		// The frame rate the frames are actually presented at goes right after the line the patch
		// draws in the bottom left corner (see overlay.h), the same as in th11 to th15
		// (the overlay readouts are Config::ShowOverlay's business; see Limiter::Initialize)
		overlay_set_present_rate_follow_text(true);
	}
}

FPSTarget th10_replay_callback() {
	if (*CReplayManager::InstancePtr && (*CReplayManager::InstancePtr)->mode == 1) {
		// TODO: Reverse engineer this struct instead of being lazy
		auto input = *(DWORD*)0x00474E30;
		if (input & InputState::Focus)
			return FPSTarget::ReplaySlow;
		else if (input & (InputState::Skip | InputState::Shoot))
			return FPSTarget::ReplaySkip;
	}
	return FPSTarget::Game;
}
