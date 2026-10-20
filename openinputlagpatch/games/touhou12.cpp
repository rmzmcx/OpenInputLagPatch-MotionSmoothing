// Touhou 12: Undefined Fantastic Object v1.00b

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
#include "touhou12.h"

using namespace Touhou12;

CReplayManager** CReplayManager::InstancePtr = (CReplayManager**)0x004C22C8;

int __stdcall th12_window_update_hook(void* self) {
	Limiter::Tick();
	return CWindowManager__UpdateFast(self);
}

// ---------------------------------------------------------------------------
// Frame interpolation, stage 0: extra presentations
// ---------------------------------------------------------------------------
//
// th12 draws a game frame the way th13, th14 and th15 do. The block that does it sits in three
// frame functions, one per kind of scene the game can be in, and all three are the same:
//
//     0x450152  mov eax, [0x4CE8F0]       the device
//     0x450159  [ecx+0xA4]                BeginScene
//     0x450162  FUN_0045A380              reset the sprite queue
//     0x450167  mov edi, 0x4CE8E8         the render context, for the call below
//     0x45016C  [0x4CF278] = 0xFF
//     0x450176  FUN_00430300              prepare the render context (its object in EDI)
//     0x45017B  FUN_00462620              render every object (they fill the sprite queue)
//     0x450180  mov esi, [0x4CE8CC]       the sprite queue's owner, for the call below
//     0x450186  FUN_0045A3C0              draw the sprite queue (its object in ESI)
//     0x450192  [ecx+0x104]               SetTexture(0, NULL)
//     0x4501A6  [ecx+0xA8]                EndScene
//
// The other two blocks are at 0x450500 and 0x45069F, with the same instructions at different
// addresses. Each frame function then calls a helper of its own right after its EndScene, and that
// is where it presents: 0x450355 in FUN_004501E0, 0x450586 in FUN_00450580 and 0x450789 in
// FUN_00450720. Every one of them is a 5 byte load of the device followed by the arguments being
// pushed and the present called over the 16 bytes after it.
//
// Two of the helpers take their object in a register instead of on the stack, which is how the
// compiler that built this game left them, so they can't be reached through a function pointer -
// the trampolines below are what put the object where each one expects it. Both registers are
// callee-saved and are put back before returning: leaving them holding the object hands the caller
// something other than what it had, which is what broke the th13 version of this code (it crashed
// in the SetTexture right after the sprite queue was drawn).
//
// The extra presentations draw the state the game is already in, which is the "moved forward by
// zero frames" case; stage 1 is what puts anything of the game's own in motion inside them.

// The plain calls the render block makes into the game
static auto th12_reset_sprite_queue = (void(*)())0x0045A380;
static auto th12_render_objects = (int(*)())0x00462620;
static void* const th12_prepare_render_address = (void*)0x00430300;
static void* const th12_draw_sprite_queue_address = (void*)0x0045A3C0;

static void __declspec(naked) th12_prepare_render() {
	__asm {
		push edi
		mov edi, 0x004CE8E8
		call dword ptr [th12_prepare_render_address]
		pop edi
		ret
	}
}

static void __declspec(naked) th12_draw_sprite_queue() {
	__asm {
		push esi
		mov esi, dword ptr ds:[0x004CE8CC]
		call dword ptr [th12_draw_sprite_queue_address]
		pop esi
		ret
	}
}

// Where the user's tool builds its frame and where it draws it. thprac's th12 module starts its
// frame from a hook on the return of the scene update at 0x4625FB and draws the overlay it built
// from a hook on the return of the object render at 0x462722 (see tool_state.h); both are the ret
// of the walk the render block above calls into.
static unsigned char* const th12_tool_frame_address = (unsigned char*)0x004625FB;
static unsigned char* const th12_tool_draw_address = (unsigned char*)0x00462722;

// The viewport the scene is drawn with, taken from the frame the game's own render ran in
static D3DVIEWPORT9 th12_scene_viewport = {};

// The two transforms the game sets before its render block (its camera setup calls SetTransform with
// D3DTS_VIEW and D3DTS_PROJECTION, from the matrices it keeps in the object at 0x4CEC04), taken from
// the frame the game's own render ran in. An extra presentation runs after the whole frame has been
// drawn and a menu sets up a camera of its own, so these are put back before the frame is drawn again
// (see th12_extra_presentation).
static D3DMATRIX th12_scene_view = {};
static D3DMATRIX th12_scene_projection = {};
static bool th12_scene_transforms = false;

// The device the game draws with. The present sites load it from the game's own global, so that is
// the device that has to present the frame - not the one the patch's D3D9 hook captured, which is
// the same object while that hook is in place and nothing at all if the game got its device in a
// way the hook doesn't see. Reporting a failed present to the game ends it (its device-lost
// handling takes over), so the device the game itself would have presented with comes first.
static IDirect3DDevice9* th12_device() {
	IDirect3DDevice9* device = *(IDirect3DDevice9**)0x004CE8F0;
	if (device != nullptr)
		return device;
	return d3d9_hooked_device();
}

// The menus draw their text through a list of records the game keeps in the text object it holds at
// [0x4B43B8]: the game's own printf-for-text call appends a record (0x138 bytes each, at +0x97C -
// FUN_004014F0 does that) and bumps the frame's count of them (+0x18F7C), and the drawing then walks
// the records that count covers (FUN_00401790). The list is started over once per frame - that is
// part of what the update walk does, which an extra presentation does not run - so an extra frame
// that draws the same screen while the list still holds the records the game's own frame appended
// draws that text a second time on top of itself, which the alpha blending these screens use turns
// into text that looks a little denser than the game's own frame shows.
//
// So the count the game's own run of the walk started from is kept here and put back before every
// extra presentation: each one then appends its own records from the start of the list, exactly the
// way the game's own frame did.
static const size_t th12_text_record_count = 0x18F7C;
static const size_t th12_text_record_array = 0x97C;
static unsigned int th12_text_records;

static unsigned int th12_text_count() {
	void* text = *(void**)0x004B43B8;
	return text != nullptr ? *(unsigned int*)((char*)text + th12_text_record_count) : 0;
}

static void th12_text_records_save() {
	th12_text_records = th12_text_count();
}

static void th12_text_records_restore() {
	void* text = *(void**)0x004B43B8;
	if (text != nullptr)
		*(unsigned int*)((char*)text + th12_text_record_count) = th12_text_records;
}

// Remembers the viewport the scene is about to be drawn with, then lets the game's own render run
// exactly as it always did (see th12_extra_presentation for what it's for)
// The screen shake: the game keeps the offset it shakes the picture by in the Float2 at 0x4CEE04 and
// zeroes it inside its own draw pass, once it has been applied (the diagnostics showed it going from
// (+-1.1,+-1.1) to (0,0) while the frame was drawn, and the picture an extra pass drew came out that
// many pixels away from the one the game's own pass drew - the same pattern as th08's and th20's
// shake). The value the game's pass started with is therefore put back around every extra pass.
static ExtraFloatState th12_shake = { (float*)0x004CEE04, 2 };

// ---------------------------------------------------------------------------
// Frame interpolation: the stage object's transition state
// ---------------------------------------------------------------------------
//
// th12 uses the same layout as th10 and th11. The stage object at [0x4B43C0] (and the second
// slot at [0x4B43BC]) has an update task (0x403EC0 -> 0x403020) and two draw tasks:
// 0x403ED0 -> 0x4036F0 and 0x403EE0 -> 0x403B60. The draw ones change the object:
//
//   0x403E0D (FUN_00403B60): if ([stage+0x35C4] > 0) FUN_00464A20(&[stage+0x35C0], -1.0f), which
//                            subtracts one from the stage clear timer
//   0x40391D (FUN_004036F0): if ([stage+0x35BC] & 4 and [stage+0x35C4] < 30)
//                            FUN_004529A0(3, 30, 0, 0, 0, 0x0B), which creates a 30 frame fade
//
// The game draws once a frame, so those callbacks run once a frame. An extra presentation runs the
// render chain again, which subtracts the timer two or three times per game frame and can create
// more than one fade for the same frame. The fade object itself is fine: its own timer runs on the
// update chain, once a frame.
//
// So an extra pass copies both stage objects (0x3724 bytes, the size the allocator uses) before
// its render walk and puts them back afterwards, and the fade factory call at 0x40391D is skipped
// while an extra is drawing.
static const size_t th12_stage_size = 0x3724;
static void** const th12_stage_slots[2] = { (void**)0x004B43BC, (void**)0x004B43C0 };
static unsigned char* th12_stage_saved[2];
static bool th12_stage_saved_valid[2];
static bool th12_in_extra;

static void th12_stage_save() {
	for (int i = 0; i < 2; ++i) {
		th12_stage_saved_valid[i] = false;
		void* stage = *th12_stage_slots[i];
		if (stage == nullptr)
			continue;
		if (th12_stage_saved[i] == nullptr)
			th12_stage_saved[i] = (unsigned char*)malloc(th12_stage_size);
		if (th12_stage_saved[i] == nullptr)
			continue;
		memcpy(th12_stage_saved[i], stage, th12_stage_size);
		th12_stage_saved_valid[i] = true;
	}
}

static void th12_stage_restore() {
	for (int i = 0; i < 2; ++i) {
		void* stage = *th12_stage_slots[i];
		if (!th12_stage_saved_valid[i] || stage == nullptr)
			continue;
		memcpy(stage, th12_stage_saved[i], th12_stage_size);
		th12_stage_saved_valid[i] = false;
	}
}

// FUN_004529A0 builds a fade object and puts its tasks on the chains (six arguments, ret 18h).
// The call from the stage's draw callback at 0x40391D is the one that creates the stage clear
// fade; an extra presentation must not create another one. The callers ignore the return value,
// so nullptr is safe; everything else goes to the game's factory.
static void* (__stdcall* th12_fade_create)(int mode, int duration, int a3, int a4, int a5, int a6) =
	(void*(__stdcall*)(int, int, int, int, int, int))0x004529A0;

static void* __stdcall th12_fade_create_extra_guard(int mode, int duration, int a3, int a4, int a5, int a6) {
	if (th12_in_extra)
		return nullptr;
	return th12_fade_create(mode, duration, a3, a4, a5, a6);
}

static void th12_render_hook() {
	IDirect3DDevice9* device = th12_device();
	if (device != nullptr) {
		device->GetViewport(&th12_scene_viewport);
		th12_scene_transforms = SUCCEEDED(device->GetTransform(D3DTS_VIEW, &th12_scene_view)) &&
			SUCCEEDED(device->GetTransform(D3DTS_PROJECTION, &th12_scene_projection));
	}

	ToolState::WatchFrameReady(th12_tool_frame_address, th12_tool_draw_address);

	// What the game's own run of the walk finds the text list at, for the extra presentations to put
	// back (see th12_text_record_count)
	th12_text_records_save();
	extra_float_state_save(&th12_shake);

	th12_render_objects();
}

// Diagnostics: press U to write the presentation timing of the last frames to oilp_diag.txt
// (what the last presentation projected, see the stage 1 sections below)
static unsigned int th12_bullets_in_play;
static unsigned int th12_bullets_moved;
static float th12_player_movement;
static unsigned int th12_sprites_untracked;
static unsigned int th12_body_sprites;
static unsigned int th12_option_sprites;
static unsigned int th12_shot_sprites;
static unsigned int th12_lasers_seen;
static unsigned int th12_lasers_projected;

static const unsigned int th12_diag_frames_max = 300;
static const unsigned int th12_diag_presents_max = 8;
struct Th12DiagFrame {
	LARGE_INTEGER base;
	unsigned int presents;
	LARGE_INTEGER present_time[th12_diag_presents_max];
};
static Th12DiagFrame th12_diag_frames[th12_diag_frames_max];
static unsigned int th12_diag_count;
static bool th12_diag_key_down;

static LARGE_INTEGER th12_perf_freq;

// Defined with the player's own state below; the dump reports what each option slot thinks its
// sprites are
static void th12_options_dump(FILE* file);

static void th12_diag_dump() {
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

	unsigned int count = th12_diag_count < th12_diag_frames_max ? th12_diag_count : th12_diag_frames_max;
	unsigned int start = th12_diag_count < th12_diag_frames_max ? 0 : th12_diag_count % th12_diag_frames_max;

	fprintf(file, "oilp frame interpolation diagnostics\n");
	fprintf(file, "logic frame rate: %.2f, configured presentation rate: %.2f\n",
		Limiter::game_fps, Limiter::PresentRate());
	fprintf(file, "frames recorded: %u\n", count);
	fprintf(file, "bullets: %u in play, %u projected forward (last game frame)\n",
		th12_bullets_in_play, th12_bullets_moved);
	fprintf(file, "player: %u body, %u option and %u shot sprite(s) projected (last game frame)\n",
		th12_body_sprites, th12_option_sprites, th12_shot_sprites);
	fprintf(file, "player: %u sprite(s) this frame had no movement record (should stay 0)\n",
		th12_sprites_untracked);
	fprintf(file, "player movement: up to %.2f px per frame since the last dump\n", th12_player_movement);
	th12_player_movement = 0.0f;
	fprintf(file, "lasers: %u in play, %u projected forward (last game frame)\n",
		th12_lasers_seen, th12_lasers_projected);
	th12_options_dump(file);

	double presents = 0.0;
	double first = 0.0;
	double last = 0.0;
	for (unsigned int i = 0; i < count; ++i) {
		const Th12DiagFrame& frame = th12_diag_frames[(start + i) % th12_diag_frames_max];
		fprintf(file, "frame %4u: %u present(s)", i, frame.presents);
		for (unsigned int p = 0; p < frame.presents && p < th12_diag_presents_max; ++p)
			fprintf(file, " [%u] %.2fms", p,
				(double)(frame.present_time[p].QuadPart - frame.base.QuadPart) * 1000.0 / (double)th12_perf_freq.QuadPart);
		fprintf(file, "\n");
		presents += frame.presents;
		if (i == 0)
			first = (double)frame.base.QuadPart;
		last = (double)frame.base.QuadPart;
	}
	if (last > first)
		fprintf(file, "average: %.3f presents per frame, %.1f presentations per second\n",
			presents / (double)count, presents * (double)th12_perf_freq.QuadPart / (last - first));

	fclose(file);
	printf("Wrote oilp_diag.txt\n");
}

// ---------------------------------------------------------------------------
// Frame interpolation, stage 1: project the bullets forward
// ---------------------------------------------------------------------------
//
// The bullets of a stage live in the bullet manager at 0x4B43C8, which the stage build
// (FUN_004097F0) allocates as a single 0x4DEBE0 byte block and FUN_00409530 then fills in: it loads
// bullet.anm into manager+0x4DEBDC (the four bytes of the block that aren't part of the pool) and
// points manager+0x10 at the pool, which starts at manager+0x64 and holds 2001 bullets of 0x9F8
// bytes each. Every bullet carries an animation object of its own at +0x08.
//
// The manager's update task (0x40A1F0) runs the bullets and rebuilds the six in-play lists at
// manager+0x14 - one head and one tail per column, so manager+0x14..0x28 are the heads and
// manager+0x2C..0x40 the tails. FUN_0040A020 walks the pool and, for every bullet that is in play
// (a non-zero 16 bit word at +0x532), runs it through FUN_004099E0 and threads it onto the list of
// the column the bullet keeps at +0x54C; the bullet's link to the next one of its column is its own
// +0x538. The manager's draw task (0x40A220) walks those same lists (FUN_0040A150 once per column),
// which makes them exactly the bullets that are drawn - so that is the list walked here, and it is
// read after the game's own update ran, which is the case by the time the frame is presented.
//
// A bullet moves by its velocity: FUN_004099E0 adds the three floats at +0x4C8/+0x4CC/+0x4D0 to the
// position at +0x4BC/+0x4C0/+0x4C4, scaled by the global 0x4B2ED0 (the factor the game scales every
// movement with, 1 unless the screen is full enough for it to slow down), once per frame. It moves
// at half of that while its state at +0x532 is 2 or 3 (0 is free, 1 is a bullet in flight; the half
// is the double at 0x4A3CF8, which is 0.5), and not at all in any other state. The states are what
// 0x409A05 switches on, and the two half-speed paths (0x409AAC for state 2, 0x409A27 for state 3)
// are the same code as the full speed one (0x409C9A) with that one factor more.
//
// The drawn sprites follow on their own: the draw pass copies this position into the sprite fields
// at +0x42C/+0x430/+0x434 and hands the animation object at +0x08 to the sprite queue (0x45C900), so
// moving the position is enough - the same rule as in th13 and th14.

static void** const th12_bullet_manager = (void**)0x004B43C8;
static const size_t th12_bullet_lists = 0x14;      // in the manager: the first column's head
static const size_t th12_bullet_columns = 6;       // ...and how many columns there are
static const size_t th12_bullet_next = 0x538;      // in a bullet: the next one of its column
static const size_t th12_bullet_position = 0x4BC;  // in a bullet: its position, 3 floats
static const size_t th12_bullet_velocity = 0x4C8;  // in a bullet: what it moves by, 3 floats
static const size_t th12_bullet_state = 0x532;     // in a bullet: what it's doing, 16 bits
// The factor the game scales every movement with (1 unless the screen is full enough for it to
// slow down)
static float* const th12_time_scale = (float*)0x004B2ED0;
// How many of the game's updates it runs per rendered frame, minus one
static unsigned char* const th12_frames_per_render = (unsigned char*)0x004CEACE;
// The object of the pause screen while it is up, and the flags of it the bullet tasks look at
static void** const th12_pause_object = (void**)0x004B44E8;
static const size_t th12_pause_flags = 0x60;

// The pool holds 2001 bullets, so the lists of the ones in play can't be longer
static const size_t th12_bullets_saved_max = 2001;
struct Th12SavedBullet {
	void* bullet;
	float position[3];
};
static Th12SavedBullet th12_saved_bullets[th12_bullets_saved_max];
static size_t th12_saved_bullets_count;

// Whether the pause screen has the picture held. A picture the game isn't updating (or isn't
// drawing) must not be moved forward either. 0x40A1F0 and 0x40A220 are the game's two bullet tasks,
// and this is the same test each of them makes.
static bool th12_bullets_held() {
	void* pause = *th12_pause_object;
	if (pause == nullptr)
		return false;
	const unsigned int flags = *(unsigned int*)((char*)pause + th12_pause_flags);
	return ((flags | (flags >> 2)) & 1) != 0 || (flags & 4) != 0;
}

// How much of a frame's movement a bullet in this state gets (0x409A05)
static float th12_bullet_step_factor(unsigned short state) {
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
static void th12_bullets_advance(float t) {
	th12_saved_bullets_count = 0;
	th12_bullets_in_play = 0;
	th12_bullets_moved = 0;

	void* manager = *th12_bullet_manager;
	if (manager == nullptr || th12_bullets_held())
		return;

	unsigned int frames_per_render = *th12_frames_per_render;
	if (frames_per_render > 2) // the game's own setting only goes to 2
		frames_per_render = 2;

	float scale = *th12_time_scale;
	if (!(scale > 0.0f) || scale > 2.0f)
		scale = 1.0f;

	const float step = t * (float)(frames_per_render + 1) * scale;

	for (size_t column = 0; column < th12_bullet_columns; ++column) {
		void* bullet = *(void**)((char*)manager + th12_bullet_lists + column * 4);
		for (size_t guard = 0; bullet != nullptr && guard < th12_bullets_saved_max; ++guard) {
			void* next = *(void**)((char*)bullet + th12_bullet_next);

			++th12_bullets_in_play;
			const float factor = th12_bullet_step_factor(*(unsigned short*)((char*)bullet + th12_bullet_state));
			if (factor != 0.0f && th12_saved_bullets_count < th12_bullets_saved_max) {
				float* position = (float*)((char*)bullet + th12_bullet_position);
				const float* velocity = (const float*)((char*)bullet + th12_bullet_velocity);
				Th12SavedBullet& saved = th12_saved_bullets[th12_saved_bullets_count++];
				saved.bullet = bullet;

				const float bullet_step = step * factor;
				for (int axis = 0; axis < 3; ++axis) {
					saved.position[axis] = position[axis];
					position[axis] += velocity[axis] * bullet_step;
				}
				++th12_bullets_moved;
			}

			bullet = next;
		}
	}
}

// Puts the positions of every bullet th12_bullets_advance moved back
static void th12_bullets_undo() {
	for (size_t i = 0; i < th12_saved_bullets_count; ++i) {
		const Th12SavedBullet& saved = th12_saved_bullets[i];
		float* position = (float*)((char*)saved.bullet + th12_bullet_position);
		for (int axis = 0; axis < 3; ++axis)
			position[axis] = saved.position[axis];
	}
	th12_saved_bullets_count = 0;
}

// ---------------------------------------------------------------------------
// Frame interpolation, stage 1: the player, its options and its shots
// ---------------------------------------------------------------------------
//
// The player of the stage that is running is at 0x4B4514 (0xC59C bytes, one animation object of its
// own at +0x14 and another at +0x4C8). Its draw (FUN_00437600, the draw task 0x437670) reads the
// three floats at +0x97C/+0x980/+0x984, copies them into the body animation object's drawn
// position - (player+0x14)+0x430 - and hands that object to the sprite queue (0x45C900) in the same
// breath, so the renderer reads exactly that position: moving it is what moves the player.
//
// Everything else the player puts on screen is positioned by the update instead, so those sprites
// are what has to move:
//
//   - the options are 8 slots of 0xE4 bytes at +0x8268. A slot keeps its position as two ints in
//     1/128 pixel units at +0x54/+0x58 (the shot spawn at 0x4396E8, which puts a shot an option
//     fires on screen, and the option's own update at 0x43A6C9 both read exactly those two), and
//     the two sprite objects it wears at +0xA8/+0xAC (0x4615A0 builds them, 0x4381E0 and 0x41D020
//     release them).
//   - the shots are 256 slots of 0x78 bytes at +0xA68. A slot that is in use has a non-zero
//     countdown at +0x38, the position the game moves at +0x04/+0x08/+0x0C, and the sprite object
//     it wears at +0x3C. The slot walk (0x439B10) copies that position into the sprite's drawn
//     position every frame, which is why the sprite is what is moved here.
//
// A sprite object - the class 0x4615A0 builds - keeps its drawn position at +0x430/+0x434/+0x438.
// That is what 0x4615A0 itself writes when it spawns one, what 0x439B10 writes for the shot it
// belongs to, and what the getter at 0x461BCE hands out. A sprite object's first dword is its own
// address, which is what the manager's two object lists (at 0x4CE8CC + 0x8856B8 and + 0x8856C0)
// are searched by, so that is how a handle a slot keeps is checked against the objects that are
// really there.
//
// None of the three have a velocity that could be read - the options follow the player and the
// shots follow whatever pattern they were fired with - so what each of them moves by in a frame is
// measured, the way th13 measures its options and its shots: every game frame the position is
// compared with the one of the frame before, and the extra presentations move the sprite by that
// difference times the fraction of a frame they stand for. The shift is undone after every
// presentation, so what gets measured is the game's own movement.

static void** const th12_player = (void**)0x004B4514;
static const size_t th12_player_position = 0x97C;        // the player's own position, 3 floats
static const size_t th12_player_options = 0x8268;        // 8 slots of 0xE4 bytes
static const size_t th12_player_option_count = 8;
static const size_t th12_player_option_size = 0xE4;
static const size_t th12_player_option_position = 0x54;  // 2 ints, in 1/128 pixels
static const size_t th12_player_option_sprite = 0xA8;    // 2 sprite objects
static const size_t th12_player_shots = 0xA68;           // 256 slots of 0x78 bytes
static const size_t th12_player_shot_count = 256;
static const size_t th12_player_shot_size = 0x78;
static const size_t th12_player_shot_live = 0x38;        // non-zero while the slot is in use
static const size_t th12_player_shot_position = 0x04;    // 3 floats
static const size_t th12_player_shot_sprite = 0x3C;      // the sprite object it wears
static const size_t th12_sprite_position = 0x430;        // in a sprite object: 3 floats
// The option positions are kept in 1/128 pixel units
static const float th12_units_per_pixel = 128.0f;
static const float th12_max_step = 48.0f;

// What one thing moved by in a frame, kept per thing the player owns. Every one of them is a fixed
// slot of the player object (the player itself, one of the 8 options, one of the 256 shots), so a
// record never has to be recycled: an empty shot slot simply has nothing to move.
struct Th12Motion {
	unsigned int frame;
	float last_x, last_y;
	float delta_x, delta_y;
};

static unsigned int th12_motion_frame = 1;

static void th12_measure(Th12Motion& motion, float x, float y) {
	// Only a thing that was measured in the frame right before this one has something to compare
	// against - one that just appeared has not moved as far as this can tell
	if (motion.frame + 1 == th12_motion_frame) {
		const float dx = x - motion.last_x;
		const float dy = y - motion.last_y;
		if (dx > -th12_max_step && dx < th12_max_step && dy > -th12_max_step && dy < th12_max_step) {
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
	motion.frame = th12_motion_frame;
	motion.last_x = x;
	motion.last_y = y;
}

// The position of the sprite object a handle names, or null if it names none that is live right
// now. A sprite object keeps its own address in its first dword and is threaded onto one of the
// manager's two object lists. This asks the game's own lookup (0x461920) instead of trusting the
// handle to be a pointer - a slot can keep a stale handle for a while after its sprite is gone, and
// dereferencing that would be a crash. That function is what the game looks its objects up with: it
// takes the handle on the stack and the manager in EDX, which a plain function pointer call cannot
// set, so it goes through this, and it cleans the handle off the stack itself (ret 4).
//
// This has to be asked *every* frame, not only when a slot's handle changes: the game frees the
// sprites of an option without always clearing the handle it left behind (0x4381E0 releases
// [slot+0xA8] and [slot+0xAC] and only zeroes another field of the slot), so a handle that still
// reads the same can name a sprite that is long gone - and caching a resolved pointer across frames
// is what crashed the first cut of this code (a read of the sprite's position through a freed
// pointer, at the top of the option loop).
static void* const th12_sprite_lookup_address = (void*)0x00461920;

static __declspec(naked) void* th12_sprite_lookup(unsigned int handle) {
	__asm {
		push edx
		mov edx, dword ptr ds:[0x004CE8CC]
		push dword ptr [esp + 8]
		call dword ptr [th12_sprite_lookup_address]
		pop edx
		ret
	}
}

static float* th12_sprite_by_handle(unsigned int handle) {
	if (handle == 0)
		return nullptr;
	void* object = th12_sprite_lookup(handle);
	if (object == nullptr)
		return nullptr;
	return (float*)((char*)object + th12_sprite_position);
}

// One thing to move for an extra presentation, and what its position was
struct Th12Shifted {
	float* position;
	float saved[3];
};

static const size_t th12_shifted_max =
	1 + th12_player_option_count * 2 + th12_player_shot_count;
static Th12Shifted th12_shifted[th12_shifted_max];
static size_t th12_shifted_count;

static void th12_shift(float* position, const Th12Motion& motion, float t) {
	if (position == nullptr || th12_shifted_count >= th12_shifted_max)
		return;
	Th12Shifted& shifted = th12_shifted[th12_shifted_count++];
	shifted.position = position;
	for (int axis = 0; axis < 3; ++axis)
		shifted.saved[axis] = position[axis];
	position[0] += motion.delta_x * t;
	position[1] += motion.delta_y * t;
}

static void th12_undo_shifts() {
	for (size_t i = 0; i < th12_shifted_count; ++i) {
		const Th12Shifted& shifted = th12_shifted[i];
		for (int axis = 0; axis < 3; ++axis)
			shifted.position[axis] = shifted.saved[axis];
	}
	th12_shifted_count = 0;
}

// The player itself: a motion record of its own, and nothing else to keep
static Th12Motion th12_body_motion;

// The options: what each of the 8 slots moved by, and the sprite objects it wears
static Th12Motion th12_option_motions[th12_player_option_count];
static unsigned int th12_option_handles[th12_player_option_count][2];
static float* th12_option_sprite_pos[th12_player_option_count][2];

// The shots: the same, one per slot of the 256
static Th12Motion th12_shot_motions[th12_player_shot_count];
static unsigned int th12_shot_handles[th12_player_shot_count];
static float* th12_shot_sprite_pos[th12_player_shot_count];

// What each option slot says the two sprites it wears are, and whether that still names one that is
// there. The game leaves the handle behind when it frees a sprite (0x4381E0 releases both and
// zeroes another field of the slot), so a slot can keep pointing at one that is gone for a while.
static void th12_options_dump(FILE* file) {
	fprintf(file, "option slots:");
	for (unsigned int i = 0; i < th12_player_option_count; ++i) {
		fprintf(file, " [%u]", i);
		for (int sprite = 0; sprite < 2; ++sprite) {
			if (th12_option_handles[i][sprite] == 0)
				fprintf(file, " -");
			else if (th12_option_sprite_pos[i][sprite] == nullptr)
				fprintf(file, " %08x(gone)", th12_option_handles[i][sprite]);
			else
				fprintf(file, " %08x(%.1f,%.1f d%.2f,%.2f)", th12_option_handles[i][sprite],
					th12_option_sprite_pos[i][sprite][0], th12_option_sprite_pos[i][sprite][1],
					th12_option_motions[i].delta_x, th12_option_motions[i].delta_y);
		}
	}
	fprintf(file, "\n");
	// The same for the first few shots that are in play, so a shot whose sprite isn't where the
	// slot says it is shows up as well
	fprintf(file, "shot slots:");
	unsigned int shown = 0;
	for (unsigned int i = 0; i < th12_player_shot_count && shown < 8; ++i) {
		if (th12_shot_handles[i] == 0)
			continue;
		fprintf(file, " [%u]", i);
		if (th12_shot_sprite_pos[i] == nullptr)
			fprintf(file, " %08x(gone)", th12_shot_handles[i]);
		else
			fprintf(file, " %08x(%.1f,%.1f d%.2f,%.2f)", th12_shot_handles[i],
				th12_shot_sprite_pos[i][0], th12_shot_sprite_pos[i][1],
				th12_shot_motions[i].delta_x, th12_shot_motions[i].delta_y);
		++shown;
	}
	if (shown == 0)
		fprintf(file, " none");
	fprintf(file, "\n");
}

// Collects what the player owns this frame and works out what it moved by since the frame before.
// This runs once per game frame, from the game's own present (see th12_present_hook).
static void th12_player_measure() {
	++th12_motion_frame;
	th12_sprites_untracked = 0;
	th12_body_sprites = 0;
	th12_option_sprites = 0;
	th12_shot_sprites = 0;

	void* player = *th12_player;
	if (player == nullptr)
		return;

	for (unsigned int i = 0; i < th12_player_shot_count; ++i) {
		const char* slot = (const char*)player + th12_player_shots + i * th12_player_shot_size;
		const unsigned int handle = *(const unsigned int*)(slot + th12_player_shot_sprite);
		if (*(const int*)(slot + th12_player_shot_live) != 0 && handle != 0) {
			const float* position = (const float*)(slot + th12_player_shot_position);
			th12_measure(th12_shot_motions[i], position[0], position[1]);
			th12_shot_handles[i] = handle;
			th12_shot_sprite_pos[i] = th12_sprite_by_handle(handle);
			if (th12_shot_sprite_pos[i] == nullptr)
				++th12_sprites_untracked;
		} else {
			th12_shot_handles[i] = 0;
			th12_shot_sprite_pos[i] = nullptr;
			th12_shot_motions[i].frame = 0;
		}
	}

	for (unsigned int i = 0; i < th12_player_option_count; ++i) {
		const char* slot = (const char*)player + th12_player_options + i * th12_player_option_size;
		const int* position = (const int*)(slot + th12_player_option_position);
		th12_measure(th12_option_motions[i],
			(float)position[0] / th12_units_per_pixel, (float)position[1] / th12_units_per_pixel);
		for (int sprite = 0; sprite < 2; ++sprite) {
			const unsigned int handle =
				*(const unsigned int*)(slot + th12_player_option_sprite + sprite * 4);
			th12_option_handles[i][sprite] = handle;
			th12_option_sprite_pos[i][sprite] = th12_sprite_by_handle(handle);
			if (handle != 0 && th12_option_sprite_pos[i][sprite] == nullptr)
				++th12_sprites_untracked;
		}
	}

	// The player itself: its position is what the update moves, and its draw copies it into the
	// sprite it wears, so that is what is measured
	{
		const float* position = (const float*)((const char*)player + th12_player_position);
		th12_measure(th12_body_motion, position[0], position[1]);
		const float x = th12_body_motion.delta_x < 0.0f ? -th12_body_motion.delta_x : th12_body_motion.delta_x;
		const float y = th12_body_motion.delta_y < 0.0f ? -th12_body_motion.delta_y : th12_body_motion.delta_y;
		const float magnitude = x > y ? x : y;
		if (magnitude > th12_player_movement)
			th12_player_movement = magnitude;
	}
}

// Moves the player and everything it put on screen t frames ahead of the state the game is in, and
// remembers what it changed
static void th12_player_advance(float t) {
	th12_body_sprites = 0;
	th12_option_sprites = 0;
	th12_shot_sprites = 0;
	th12_shifted_count = 0;

	if (th12_bullets_held())
		return;

	void* player = *th12_player;
	if (player == nullptr)
		return;

	if (th12_motion_frame == th12_body_motion.frame) {
		th12_shift((float*)((char*)player + th12_player_position), th12_body_motion, t);
		++th12_body_sprites;
	}

	for (unsigned int i = 0; i < th12_player_option_count; ++i) {
		if (th12_motion_frame != th12_option_motions[i].frame)
			continue;
		for (int sprite = 0; sprite < 2; ++sprite) {
			if (th12_option_sprite_pos[i][sprite] == nullptr)
				continue;
			th12_shift(th12_option_sprite_pos[i][sprite], th12_option_motions[i], t);
			++th12_option_sprites;
		}
	}

	for (unsigned int i = 0; i < th12_player_shot_count; ++i) {
		if (th12_shot_sprite_pos[i] == nullptr || th12_motion_frame != th12_shot_motions[i].frame)
			continue;
		th12_shift(th12_shot_sprite_pos[i], th12_shot_motions[i], t);
		++th12_shot_sprites;
	}
}

// ---------------------------------------------------------------------------
// Frame interpolation, stage 1: the straight lasers
// ---------------------------------------------------------------------------
//
// The lasers of a stage live in a manager of their own - [0x4B44F4], 0x490 bytes, allocated by
// FUN_004281C0 and filled in by FUN_00427FD0 - and the manager's two tasks both walk one list: the
// update task (0x4283D0) and the draw task (0x428430), which starts from the root node's next at
// manager+0x18 and follows each laser's own next at +0x08. The links are the object's own, exactly
// like th13's lasers (+0x04 previous, +0x08 next), and the draw skips a laser whose state at +0x0C
// is 1 - that is what the spawner 0x428450 threads new lasers in with and what the update's switch
// at 0x42AEEE reads.
//
// A laser's draw is what fixes which of its fields has to move. The Line laser's (FUN_00429AF0,
// vtable+0x0C) and the infinite one's (FUN_0042B100, the same function one class over) both copy
// the position at +0x50/+0x54/+0x58 into the sprites they queue and turn the angle at +0x68 into
// the value they hand those sprites (into +0x668 and +0x68C respectively), so the position and the
// angle are what the renderer reads. The length at +0x6C is what their updates grow: the Line one
// by the rate at +0x74, clamped to what is left of +0x464, and the infinite one by the same +0x74
// up to +0x474.
//
// Which lasers are straight is a whitelist of their vtables, taken from the RTTI the game carries:
// LaserLineInf (0x4A0654) and LaserInfiniteInf (0x4A0704) are in it, LaserCurveInf (0x4A06AC) is
// left out as the requirements ask, and the manager's root node has no vtable of its own so it
// never matches.
//
// What each of them moves by is read straight out of the laser, but the two classes keep it in
// different places, so the layout is picked by vtable along with which classes are straight at all:
//
//   LaserLineInf      velocity +0x5C/+0x60/+0x64, length grows by the rate at +0x74 up to what is
//                     left of +0x46C (the update subtracts the grown +0x78 from it), and it does
//                     not turn on its own - its update (FUN_00429610) never writes the angle.
//   LaserInfiniteInf  velocity +0x460/+0x464/+0x468, the angle turns by +0x470 and the length grows
//                     by the same +0x74 up to +0x474.
//
// Reading the velocity rather than measuring what the last frame moved by is what this does now,
// and the difference matters: a measured step is only the previous frame's step of *whatever laser
// was at that address*, and these are short-lived objects the game recycles (a stage with a few
// hundred lasers in play allocates and frees them constantly), so a fresh laser that took a dead
// one's slot would be drawn offset by the difference of two unrelated positions. A velocity read
// off the laser that is there now cannot do that, and a laser whose velocity really is zero - the
// warning lines are exactly that - is left where it is instead of inheriting anyone's movement.

// One class of straight laser: where it keeps what it moves by, and what it grows towards
struct Th12LaserLayout {
	const void* vtable;
	size_t velocity;            // 3 floats
	size_t angle_speed;         // 0 for a class that doesn't turn
	size_t length_speed;
	size_t length_limit;
	size_t length_limit_subtract;
};

static void** const th12_laser_manager = (void**)0x004B44F4;
static const size_t th12_laser_list = 0x18;      // in the manager: the first laser of the list
static const size_t th12_laser_next = 0x08;      // in a laser: the next one
static const size_t th12_laser_state = 0x0C;     // in a laser: 1 while it isn't drawn
static const size_t th12_laser_position = 0x50;  // where it starts, 3 floats
static const size_t th12_laser_angle = 0x68;     // the direction it points in, radians
static const size_t th12_laser_length = 0x6C;    // how far it reaches

static const size_t th12_lasers_max = 0x100 + 1; // the manager's own limit, plus its root entry
static const Th12LaserLayout th12_laser_layouts[] = {
	// LaserLineInf: it doesn't turn on its own, and its length stops at what is left of +0x46C
	{ (const void*)0x004A0654, 0x5C,  0,    0x74, 0x46C, 0x78 },
	// LaserInfiniteInf: turns by +0x470, and its length stops at +0x474
	{ (const void*)0x004A0704, 0x460, 0x470, 0x74, 0x474, 0    },
};

// The layout of the class this laser is, or null if it isn't one of the straight ones (the curve
// laser is deliberately not one of them)
static const Th12LaserLayout* th12_laser_layout(const void* laser) {
	const void* vtable = *(const void* const*)laser;
	for (size_t i = 0; i < sizeof(th12_laser_layouts) / sizeof(th12_laser_layouts[0]); ++i) {
		if (vtable == th12_laser_layouts[i].vtable)
			return &th12_laser_layouts[i];
	}
	return nullptr;
}

struct Th12LaserMoved {
	void* laser;
	float position[3];
	float angle, length;
};

static Th12LaserMoved th12_lasers_moved[th12_lasers_max];
static size_t th12_lasers_moved_count;

// Moves the straight lasers t frames ahead of the state the game is in, and remembers what it
// changed. Each laser is put where the velocity it carries right now would have it t frames from
// here: the game's own update adds velocity * the global scale to the position once per frame, so
// this is that same step, taken for the fraction of a frame this presentation stands for.
static void th12_lasers_advance(float t) {
	th12_lasers_moved_count = 0;
	th12_lasers_seen = 0;
	th12_lasers_projected = 0;

	void* manager = *th12_laser_manager;
	if (manager == nullptr || th12_bullets_held())
		return;

	unsigned int frames_per_render = *th12_frames_per_render;
	if (frames_per_render > 2) // the game's own setting only goes to 2
		frames_per_render = 2;

	float scale = *th12_time_scale;
	if (!(scale > 0.0f) || scale > 2.0f)
		scale = 1.0f;

	const float step = t * (float)(frames_per_render + 1) * scale;

	void* laser = *(void**)((char*)manager + th12_laser_list);
	// The list is the game's own and this runs where nothing can change it; the bound is only so a
	// list that got broken some other way can't run away with the frame
	for (size_t guard = 0; laser != nullptr && guard < th12_lasers_max * 4; ++guard) {
		void* next = *(void**)((char*)laser + th12_laser_next);
		const Th12LaserLayout* layout = th12_laser_layout(laser);
		// One whose state is 1 isn't drawn yet (the draw walk skips those)
		if (layout != nullptr && *(const int*)((char*)laser + th12_laser_state) != 1) {
			++th12_lasers_seen;
			if (th12_lasers_moved_count < th12_lasers_max) {
				Th12LaserMoved& moved = th12_lasers_moved[th12_lasers_moved_count++];
				moved.laser = laser;

				float* position = (float*)((char*)laser + th12_laser_position);
				float* angle = (float*)((char*)laser + th12_laser_angle);
				float* length = (float*)((char*)laser + th12_laser_length);
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
					// (the Line one at what is left of +0x46C once the grown +0x78 is taken off it,
					// the infinite one at +0x474)
					float limit = *(const float*)((char*)laser + layout->length_limit);
					if (layout->length_limit_subtract != 0)
						limit -= *(const float*)((char*)laser + layout->length_limit_subtract);
					*length += *(const float*)((char*)laser + layout->length_speed) * step;
					if (limit > 0.0f && *length > limit)
						*length = limit;
				}
				++th12_lasers_projected;
			}
		}
		laser = next;
	}
}

static void th12_lasers_undo() {
	for (size_t i = 0; i < th12_lasers_moved_count; ++i) {
		const Th12LaserMoved& moved = th12_lasers_moved[i];
		float* position = (float*)((char*)moved.laser + th12_laser_position);
		for (int axis = 0; axis < 3; ++axis)
			position[axis] = moved.position[axis];
		*(float*)((char*)moved.laser + th12_laser_angle) = moved.angle;
		*(float*)((char*)moved.laser + th12_laser_length) = moved.length;
	}
	th12_lasers_moved_count = 0;
}

// The camera setup (0x430910) takes its object - the one the game keeps at 0x4CEC04 - in EDI, which a
// plain call cannot set, so it goes through this. EDI is callee-saved, so the caller's value is put
// back before returning.
static void* const th12_camera_setup_address = (void*)0x00430910;

static void __declspec(naked) th12_camera_setup() {
	__asm {
		push edi
		mov edi, 0x004CEC04
		call dword ptr [th12_camera_setup_address]
		pop edi
		ret
	}
}

// One extra presentation of the frame: the same render block the game runs, once more, so the state
// the game is in is drawn and presented again. The tool's frame is readied again, so the object
// render below draws its overlay into this presentation the same way it does in the game's own one.
static void th12_extra_presentation(IDirect3DDevice9* device, float t) {
	device->BeginScene();
	if (th12_scene_transforms) {
		device->SetTransform(D3DTS_VIEW, &th12_scene_view);
		device->SetTransform(D3DTS_PROJECTION, &th12_scene_projection);
	}
	if (th12_scene_viewport.Width != 0)
		device->SetViewport(&th12_scene_viewport);
	// The game sets its camera up before its render block and a menu sets those up differently for
	// itself, so an extra presentation calls the game's own setup again to get the transforms above
	// plus the two values it leaves in the sprite manager that place the queue on screen.
	//
	// The queue is emptied first: the setup draws the queue once on its way in, and with nothing
	// queued that draw does nothing. The render block below empties it again anyway.
	th12_reset_sprite_queue();
	th12_camera_setup();
	th12_reset_sprite_queue();
	*(DWORD*)0x004CF278 = 0xFF;
	th12_prepare_render();
	ToolState::ReadyToDraw();
	// Everything is drawn where it will be t frames from now; the render bakes those positions into
	// what it draws, so they can all go back right after it
	th12_bullets_advance(t);
	th12_player_advance(t);
	th12_lasers_advance(t);
	// The text list goes back to how the game's own run of the walk found it, so this presentation
	// draws the same text that one drew (see th12_text_record_count above)
	th12_text_records_restore();
	// The stage's draw callbacks advance its transition (see th12_stage_save); put the object back
	// afterwards so an extra pass cannot move the transition a second or third time
	th12_stage_save();
	th12_in_extra = true;
	// Put the shake back for this pass (see the comment above)
	extra_float_state_begin(&th12_shake);
	th12_render_objects();
	extra_float_state_end(&th12_shake);
	th12_in_extra = false;
	th12_stage_restore();
	th12_lasers_undo();
	th12_undo_shifts();
	th12_bullets_undo();
	th12_draw_sprite_queue();
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
static HRESULT th12_present_hook() {
	IDirect3DDevice9* device = th12_device();
	if (device == nullptr)
		return E_FAIL;

	static bool freq_queried = false;
	if (!freq_queried) {
		freq_queried = true;
		QueryPerformanceFrequency(&th12_perf_freq);
	}

	LARGE_INTEGER base;
	QueryPerformanceCounter(&base);

	// The game's own update has run and it is about to put that frame on screen: this is the one
	// point per frame where the movement of the player, its options and its shots can be measured
	th12_player_measure();

	HRESULT result = device->Present(nullptr, nullptr, nullptr, nullptr);
	overlay_mark_presentation();
	Limiter::MarkFrameWorkEnd();
	ToolState::WatchFrameDrawn();

	unsigned int presents = 1;
	LARGE_INTEGER present_time[th12_diag_presents_max];
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

				if (presents < th12_diag_presents_max)
					QueryPerformanceCounter(&present_time[presents]);
				++presents;

				// Where this presentation sits between the game's own one and the next one:
				// the frames of one game frame are spread evenly over it, so the k-th of
				// extras + 1 of them draws the state k/(extras + 1) frames along
				th12_extra_presentation(device, (float)k / (float)(extras + 1));
			}
		}
	}

	{
		Th12DiagFrame& frame = th12_diag_frames[th12_diag_count % th12_diag_frames_max];
		frame.base = base;
		frame.presents = presents;
		for (unsigned int p = 0; p < th12_diag_presents_max; ++p)
			frame.present_time[p] = p < presents ? present_time[p] : base;
		++th12_diag_count;
	}

	if (GetAsyncKeyState('U') & 0x8000) {
		if (!th12_diag_key_down) {
			th12_diag_key_down = true;
			th12_diag_dump();
		}
	} else {
		th12_diag_key_down = false;
	}

	return result;
}

// Jumps over the present the game sets up after its present call site, so the code after it runs
// with the result the hook left in eax.
static void th12_skip_present_setup(DWORD address, size_t size) {
	BYTE patch[32];
	patch[0] = 0xE9;
	*(DWORD*)(patch + 1) = (address + size) - (address + 5);
	for (size_t i = 5; i < size; ++i)
		patch[i] = 0x90;
	patch_bytes(address, patch, size);
}

void th12_install_patches() {
	{
		// Skip the original frame limiter
		BYTE patch[] = { 0xEB, 0x43 };
		patch_bytes(0x00450424, patch, sizeof(patch));
	}
	{
		// Force fast input latency mode
		BYTE patch[] = { 0xEB };
		patch_bytes(0x0044F87A, patch, sizeof(patch)); // Skip over automatic
		patch_bytes(0x0044F88E, patch, sizeof(patch)); // Skip over normal
	}
	{
		// Hook window update
		patch_call(0x0044F8AA, th12_window_update_hook);
	}
	if (Config::D3D9Ex) {
		// Redirect Direct3DCreate9 call
		// IAT is being hooked, but thcrap also hooks Direct3DCreate9 in the same place
		// This should force our Direct3DCreate9 hook to be loaded no matter what
		patch_call(0x0044F6FC, Direct3DCreate9_hook);
	}
	if (Config::ReplaySpeedControl) {
		// Skip the original replay speed control stuff
		BYTE patch[] = { 0xEB };
		patch_bytes(0x0043C54F, patch, sizeof(patch));
	}
	if (Config::Interpolation || Config::InterpolationFPS) {
		// Take over the object render, only to remember the viewport the scene is drawn with (see
		// th12_render_hook); the render itself runs exactly the way it always did
		static const DWORD render_calls[] = { 0x0045017B, 0x00450529, 0x004506C8 };
		unsigned int render_patched = 0;
		for (DWORD address : render_calls) {
			BYTE* render_call = (BYTE*)address;
			if (render_call[0] == 0xE8) {
				patch_call(address, (void*)th12_render_hook);
				++render_patched;
			} else {
				printf("th12 render call at 0x%x doesn't look like a known one, skipping!\n", address);
			}
		}

		// The stage's draw callback FUN_004036F0 creates the stage clear fade at 0x40391D; an
		// extra presentation must skip that (see th12_fade_create_extra_guard)
		BYTE* fade_call = (BYTE*)0x0040391D;
		if (fade_call[0] == 0xE8 &&
			(DWORD)0x00403922 + *(DWORD*)(fade_call + 1) == 0x004529A0) {
			patch_call(0x0040391D, (void*)th12_fade_create_extra_guard);
		} else {
			printf("th12 fade creation call at 0x40391D doesn't look like the known one, skipping!\n");
		}

		// Take over the presents in the three frame functions' helpers. Each site starts by loading
		// the device (5 bytes, exactly what a hooked call needs) and then pushes its arguments and
		// calls Present over the 16 bytes below, which are skipped over so the code after them runs
		// with the result the hook left in eax.
		static const DWORD present_sites[] = { 0x00450355, 0x00450586, 0x00450789 };
		unsigned int present_patched = 0;
		for (DWORD address : present_sites) {
			BYTE* present_site = (BYTE*)address;
			if (present_site[0] == 0xA1 && *(DWORD*)(present_site + 1) == 0x004CE8F0) {
				patch_call(address, (void*)th12_present_hook);
				th12_skip_present_setup(address + 5, 16);
				++present_patched;
			} else {
				printf("th12 present call at 0x%x doesn't look like a known one, skipping!\n", address);
			}
		}
		printf("Frame interpolation: %u render call(s) and %u present call(s) taken over (setting %d)\n",
			render_patched, present_patched,
			Config::InterpolationFPS ? (int)Config::InterpolationFPS : Config::Interpolation);

		// The frame rate the frames are actually presented at goes right after the line the patch
		// draws in the bottom left corner (see overlay.h), the same as in th13, th14 and th15
		// (the overlay readouts are Config::ShowOverlay's business; see Limiter::Initialize)
		overlay_set_present_rate_follow_text(true);
	}
}

FPSTarget th12_replay_callback() {
	if (*CReplayManager::InstancePtr && (*CReplayManager::InstancePtr)->mode == 1) {
		// TODO: Reverse engineer this struct instead of being lazy
		auto input = *(DWORD*)0x004D48B8;
		if (input & InputState::Focus)
			return FPSTarget::ReplaySlow;
		else if (input & (InputState::Skip | InputState::Shoot))
			return FPSTarget::ReplaySkip;
	}
	return FPSTarget::Game;
}
