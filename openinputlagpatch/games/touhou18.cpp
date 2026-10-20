// Touhou 18: Unconnected Marketeers v1.00a

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
#include "touhou18.h"

using namespace Touhou18;

CReplayManager** CReplayManager::InstancePtr = (CReplayManager**)0x004CF418;

int __fastcall th18_window_update_hook(void* self) {
	Limiter::Tick();
	return CWindowManager__UpdateFast(self);
}

// ---------------------------------------------------------------------------
// Frame interpolation, stage 0: extra presentations
// ---------------------------------------------------------------------------
//
// th18 runs on the same family of engine as th17, but it is one generation further along: the
// block that draws a frame is recognisably the same idea, laid out slightly differently. It sits
// in the frame function of whatever scene is running:
//
//     0x471B87  mov eax, [0x4CCDF8]       the device
//     0x471B8F  CALL [ecx+0xA4]           BeginScene
//     0x471B95  FUN_0047E6E0              reset the sprite queue
//     0x471B9A  mov ecx, [0x51F65C]       the sprite manager
//     0x471BA0  mov [0x4CD918], 0FFh      the flag the block sets to 0xFF
//     0x471BAA  FUN_0047E730              draw the sprite queue (ECX = the sprite manager)
//     0x471BB6  mov [0x4CD918], 0
//     0x471BC5  CALL [ecx+0xE4]           SetRenderState(D3DRS_FOGENABLE, 0)
//     0x471BCB  FUN_00401420              render every object (they fill the sprite queue)
//     0x471BD6  FUN_0047E730              draw the sprite queue
//     0x471BE7  CALL [ecx+0x104]          SetTexture(0, NULL)
//     0x471BF5  CALL [ecx+0xA8]           EndScene
//
// Two things differ from th17's block: there is no "prepare the render" call into the scene object
// (the game sets D3DRS_FOGENABLE itself, which is what that call used to do), and the sprite queue
// is drawn twice - once before the objects are rendered and once after. The first of those draws
// an empty queue (the reset just emptied it), so it does nothing, but the extra presentation
// reproduces the block as it is rather than as it is meant.
//
// The camera is laid out differently too. There is no call that takes a camera object: the frame
// function loads the render context out of 0x4CD5DC and hands the device the viewport at its
// +0x10C, and copies the two floats that place the sprite queue on screen (context +0x104/+0x108,
// integers turned into floats) into the sprite manager's +0xD8/+0xDC. No view or projection
// transform is set anywhere in these frame functions, but what the scene is drawn with is taken
// from the device all the same, so an extra presentation can't be the thing that changes it.
//
// The block appears three times, once for the game scene and once for each of the two menu scenes:
// the renders are at 0x471BCB, 0x472F21 and 0x47307A, and the presentations - the game's own
// Present call - at 0x472D2D, 0x472F6C and 0x473139. Each present site is laid out identically:
// the device is loaded into eax, then the arguments are pushed and the present is called through
// the device's vtable, 14 bytes in all after the load. All five of those arguments are pushed
// inside those 14 bytes, so skipping the setup leaves the stack the way the game expects and the
// hook's return value lands in eax where the game's own code reads it.
//
// zun's engine separates the game's logic from its rendering, so the same state can be drawn again
// by running the render block again - that's what the extra presentations are. The game's own
// presentation is the first one, and the extra ones are spread evenly over the rest of the frame,
// right where the game would otherwise sit and wait for the next one, so they land on the display's
// refresh evenly.
//
// The projection of the drawn objects - moving them forward in time before the extra presentations
// - comes with stage 1, below. Stage 0 alone draws the state the game is already in, which is the
// "moved forward by zero frames" case, and is what the infrastructure - the pacing, the
// re-rendering, the overlays of the user's tools, the diagnostics - was validated with.

// The plain calls the render block makes into the game
static auto th18_reset_sprite_queue = (void(*)())0x0047E6E0;
static auto th18_draw_sprite_queue = (void(__fastcall*)(void*))0x0047E730; // ECX = the sprite manager
static auto th18_render_objects = (void(*)())0x00401420;                   // renders every object

static void** const th18_sprite_manager = (void**)0x0051F65C;
// The dword the render block sets to 0xFF (and back to 0) around its first draw of the queue
static DWORD* const th18_render_flag = (DWORD*)0x004CD918;

// The two floats the frame function copies into the sprite manager to place the queue on screen
// (the sprite manager gets them at +0xD8/+0xDC)
static const size_t th18_sprite_queue = 0xD8;
static float th18_sprite_queue_saved[2];

// What the scene is drawn with, taken from the device in the render hook (the frame functions set
// the viewport themselves and the menus set their own, so an extra presentation has to put back
// whatever the scene the game just drew was using)
static D3DVIEWPORT9 th18_scene_viewport = {};
static D3DMATRIX th18_scene_view = {};
static D3DMATRIX th18_scene_projection = {};
static bool th18_scene_transforms = false;

// Where the user's tool builds its frame and where it draws it. thprac's th18 module starts its
// frame (its NewFrame, its UI code, and its EndFrame) from a hook on the return of the scene
// update at 0x4013F5, and draws the overlay it built from a hook on the return of the object
// render at 0x401510. The extra presentations render the scene again, which erases the overlay the
// game's own pass drew, so the tool has to draw it again for every one of them (see tool_state.h -
// its frame state is found at runtime and set again there, which draws the overlay without running
// any of the tool's own code).
static unsigned char* const th18_tool_frame_address = (unsigned char*)0x004013F5;
static unsigned char* const th18_tool_draw_address = (unsigned char*)0x00401510;

// The menus draw their text through a list of records the game keeps in the manager it holds at
// [0x4CF2AC] (thprac calls the global the ASCII manager): drawing a string appends a record (0x138
// bytes each, at +0xC24 - the append is 0x419B90, which refuses to go past 0x140 of them) and
// bumps the frame's count of them (+0x19224), and the manager's own draw (0x41A180, the task
// 0x419940 the setup registers with the draw chain) then walks the records that count covers. The
// appends happen while the frame is being drawn, so an extra presentation that draws the same
// screen appends its own records after the ones the game's own pass left behind and draws both -
// the same text twice, which the alpha blending these screens use turns into text that looks a
// little denser than the game's own frame shows, or, where the second batch is placed differently,
// into a second copy of it.
//
// The count is put back to what the game's own pass started from before every extra presentation:
// each one then appends its own records from the start of the list, exactly the way the game's own
// frame did, and what it leaves behind is what the game's own pass left. The count being reset
// every frame is the other half of it: that happens in the update pass, which the extra
// presentations don't replay (the manager's aging task, 0x4198B0, is registered with the update
// chain at 0x4195AA, and it is what compacts the records and writes the surviving count back).
//
// The other values the appends touch (+0x19228, +0x1922B, +0x19244 and so on) are the layout
// style of the next record, set by the caller before each append; an extra presentation runs the
// same drawing code and sets them to the same values again, so the count is all that has to go
// back (the same holds for th12, th14, th15 and th16; th13 needed a handful of them, and if th18's
// text still comes out a little shifted that is where to look next).
static void** const th18_text_object = (void**)0x004CF2AC;
static const size_t th18_text_record_count = 0x19224;
static unsigned int th18_text_records;

static unsigned int th18_text_count() {
	void* text = *th18_text_object;
	return text != nullptr ? *(unsigned int*)((char*)text + th18_text_record_count) : 0;
}

static void th18_text_records_save() {
	th18_text_records = th18_text_count();
}

static void th18_text_records_restore() {
	void* text = *th18_text_object;
	if (text != nullptr)
		*(unsigned int*)((char*)text + th18_text_record_count) = th18_text_records;
}

// Remembers the viewport, the transforms and the two queue floats the scene is about to be drawn
// with, then lets the game's own render run exactly as it always did
// The screen shake: the game keeps the offset it shakes the picture by in the Float2 at 0x4CD574 and
// zeroes it inside its own draw pass, once it has been applied (the diagnostics showed it going from
// the shake offset to 0 while the frame was drawn, and the picture an extra pass drew came out that
// many pixels away from the one the game's own pass drew - the same pattern as th08's and th20's
// shake). The value the game's pass started with is therefore put back around every extra pass.
static ExtraFloatState th18_shake = { (float*)0x004CD574, 2 };

// ---------------------------------------------------------------------------
// Frame interpolation: the stage object's transition state
// ---------------------------------------------------------------------------
//
// The stage object at [0x4CF2B0] (and the second slot at [0x4CF2B4], 0x34A0 bytes) is drawn by
// the callback at 0x41C2A0, which changes the object in two places:
//
//   0x41C995: if ([stage+0x347C] > 0) it subtracts one from the stage clear timer at
//             [stage+0x3478]
//   0x41C481: if ([stage+0x3474] & 4 and [stage+0x347C] < 30) it allocates the 0x40 byte fade
//             object at 0x41C490 and registers its tasks
//
// The game draws once a frame, so those callbacks run once a frame. An extra presentation runs the
// render chain again, which subtracts the timer two or three times per game frame and can create
// more than one fade for the same frame. The fade object itself is fine: its own timer runs on the
// update chain, once a frame.
//
// Only the transition fields are put back, not the whole object: restoring the object's pointers
// put back handles to objects the draw pass may have replaced, which crashed when the next stage
// loaded. The two ranges are the flag/timer block at +0x3460 and the fade colour/alpha byte at
// +0x3440.
static const size_t th18_stage_ranges[][2] = { { 0x3460, 0x40 }, { 0x3440, 0x10 } };
static const size_t th18_stage_range_count = sizeof(th18_stage_ranges) / sizeof(th18_stage_ranges[0]);
static const size_t th18_stage_saved_size = 0x50;
static void** const th18_stage_slots[2] = { (void**)0x004CF2B0, (void**)0x004CF2B4 };
static unsigned char* th18_stage_saved[2];
static bool th18_stage_saved_valid[2];
static bool th18_in_extra;

static void th18_stage_save() {
	for (int i = 0; i < 2; ++i) {
		th18_stage_saved_valid[i] = false;
		void* stage = *th18_stage_slots[i];
		if (stage == nullptr)
			continue;
		if (th18_stage_saved[i] == nullptr)
			th18_stage_saved[i] = (unsigned char*)malloc(th18_stage_saved_size);
		if (th18_stage_saved[i] == nullptr)
			continue;
		size_t offset = 0;
		for (size_t r = 0; r < th18_stage_range_count; ++r) {
			memcpy(th18_stage_saved[i] + offset, (char*)stage + th18_stage_ranges[r][0],
				th18_stage_ranges[r][1]);
			offset += th18_stage_ranges[r][1];
		}
		th18_stage_saved_valid[i] = true;
	}
}

static void th18_stage_restore() {
	for (int i = 0; i < 2; ++i) {
		void* stage = *th18_stage_slots[i];
		if (!th18_stage_saved_valid[i] || stage == nullptr)
			continue;
		size_t offset = 0;
		for (size_t r = 0; r < th18_stage_range_count; ++r) {
			memcpy((char*)stage + th18_stage_ranges[r][0], th18_stage_saved[i] + offset,
				th18_stage_ranges[r][1]);
			offset += th18_stage_ranges[r][1];
		}
		th18_stage_saved_valid[i] = false;
	}
}

// The fade creation block does not check its allocation for null (it dereferences the result
// immediately), so it cannot be skipped by making the allocation fail. Instead the branch that
// guards the block (0x41C488, `jge 0x41C5A3`) is replaced with a jump to this trampoline: during an
// extra pass it always takes the "no fade" path; otherwise it runs the game's own condition.
static __declspec(noinline) bool th18_is_extra() {
	return th18_in_extra;
}

static void __declspec(naked) th18_fade_create_branch() {
	__asm {
		push eax
		push ecx
		push edx
		call th18_is_extra
		test al, al
		pop edx
		pop ecx
		pop eax
		jne skip
		cmp dword ptr [edi + 347Ch], 1Eh
		jge skip
		mov ecx, 0041C48Eh
		jmp ecx
	skip:
		mov ecx, 0041C5A3h
		jmp ecx
	}
}

static void th18_render_hook() {
	IDirect3DDevice9* device = d3d9_hooked_device();
	if (device != nullptr) {
		device->GetViewport(&th18_scene_viewport);
		th18_scene_transforms = SUCCEEDED(device->GetTransform(D3DTS_VIEW, &th18_scene_view)) &&
			SUCCEEDED(device->GetTransform(D3DTS_PROJECTION, &th18_scene_projection));
	}

	void* sprites = *th18_sprite_manager;
	if (sprites != nullptr) {
		th18_sprite_queue_saved[0] = *(float*)((char*)sprites + th18_sprite_queue);
		th18_sprite_queue_saved[1] = *(float*)((char*)sprites + th18_sprite_queue + 4);
	}

	// The tool's frame is built by now (it runs from the scene update's return, before this) but
	// not drawn yet (that happens on the return of the render below): the point to watch it from
	ToolState::WatchFrameReady(th18_tool_frame_address, th18_tool_draw_address);

	// What the game's own run of the walk finds the text list at, for the extra presentations to
	// put back (see th18_text_record_count)
	th18_text_records_save();
	extra_float_state_save(&th18_shake);

	th18_render_objects();
}

// ---------------------------------------------------------------------------
// Frame interpolation, stage 1: project the bullets forward
// ---------------------------------------------------------------------------
//
// The bullets of a stage live in the bullet manager, which FUN_00423AF0 builds and puts in
// 0x4CF2BC (it loads bullet.anm; the constructor is FUN_00423CE0). The manager owns one pool of
// 2001 bullets of 0xFA0 bytes each, starting at manager+0xEC (0xEC + 2001 * 0xFA0 = 0x7A228C,
// where the array of 2001 per-bullet words the constructor clears begins; the whole object is
// 0x7A41F8 bytes). It threads the bullets that are in play onto a list of their own: the list
// header is at manager+0xBC, its first node at manager+0xC0, and the node is part of the bullet
// itself (bullet+0x10, with the bullet at node[0] and the next node at node[4]) - which is how the
// game's own update walks it (0x424C50: `mov esi, [edi+0xC0h]` / `mov eax, [esi+4]` /
// `mov esi, [esi]`), and how a new bullet is threaded onto it (FUN_00424FE0: `add ecx, 0BCh` /
// `lea edx, [edi+10h]` / `mov [ecx+4], edx`).
//
// A bullet keeps three floats of position (+0x638) and three of velocity (+0x644). The game moves
// the position by the velocity once per frame - the three state handlers in FUN_00423E10 all end
// in the same `position += velocity * scale`:
//
//     0x423F17 (state 3)  pos += vel * [0x4CCBF0] * 0.5
//     0x423F84 (state 2)  pos += vel * [0x4CCBF0] * 0.5, then the transition to state 1
//     0x4245A9 (state 1)  pos += vel * [0x4CCBF0], unless the bullet's 0x200 flag is set
//
// where 0x4CCBF0 is the scale the game applies to every movement while the screen is full (1.0
// until it slows the bullets down), and 0.5 is the constant at 0x4B9138. It is the 16 bit state at
// +0xF68 that decides which of those a bullet gets (0x423EF1: `movzx eax, word [esi+0F68h]`,
// then sub 1 / sub 1 / sub 1), and anything else doesn't move at all. A bullet whose 0x200 flag is
// set is held: its own update skips the move and the draw pass leaves it out of the layers it
// draws (0x424D78 in the update walk, and the draw walk skips the same bit).
//
// Two things about the game matter here and are reproduced:
//
//   - The game only renders every frames_per_render + 1 of its updates (the byte at 0x4CD00C,
//     which the three frame functions compare their counter against), so the step between two
//     rendered frames is that many of its update steps. The byte is never written in this game, so
//     it stays 0, but the code keeps the general form the other games have.
//   - While the picture is held the bullets are neither updated nor drawn. th18 tests the game
//     thread object at 0x4CF2E4 for that (its flags are at +0xB0): the update task (0x424E70)
//     stops for the low bits, the draw task (0x424EB0) for bit 2, and the walk itself skips every
//     bullet's own update for bit 0x400. The ability shop (0x4CF2A4) stops the update task as
//     well, so a shop the player has open also holds the bullets.
//
// The drawn sprites follow the position on their own: the bullet draw pass (0x424EB0) copies the
// position (+0x638) into the animation object the bullet carries (+0x618) before handing it to the
// sprite queue.

// The bullet manager of the stage that is running (null while no stage is)
static void** const th18_bullet_manager = (void**)0x004CF2BC;
static const size_t th18_bullet_list_head = 0xC0; // in it: the first node of the bullets in play
static const size_t th18_bullet_list_next = 0x04; // in a node: the next node
static const size_t th18_bullet_position = 0x638; // in a bullet: its position, 3 floats
static const size_t th18_bullet_velocity = 0x644; // in a bullet: what it moves by, 3 floats
static const size_t th18_bullet_state = 0xF68;    // in a bullet: what it's doing, 16 bits
static const size_t th18_bullet_flags = 0x20;     // in a bullet: 0x200 holds it (and hides it)
// The factor the game scales every movement with (1 unless the screen is full enough for it to
// slow down)
static float* const th18_time_scale = (float*)0x004CCBF0;
// How many of the game's updates it runs per rendered frame, minus one
static unsigned char* const th18_frames_per_render = (unsigned char*)0x004CD00C;
// The game thread object the bullet tasks test for a held picture, and the flags they look at
static void** const th18_pause_object = (void**)0x004CF2E4;
static const size_t th18_pause_flags = 0xB0;
// The ability shop - while the player has it open the bullets stop moving
static void** const th18_shop_object = (void**)0x004CF2A4;

// The bullets that were moved for the presentation being drawn, and the positions they had. The
// pool holds 2001 of them, so the list of the ones in play can't be longer; only the bullets that
// were saved are moved, so a list that somehow grew past the array would only cost the projection
// of the bullets at its end.
static const size_t th18_bullets_saved_max = 2001;
struct Th18SavedBullet {
	void* bullet;
	float position[3];
};
static Th18SavedBullet th18_saved_bullets[th18_bullets_saved_max];
static size_t th18_saved_bullets_count;

// What the last presentation projected, for the diagnostics (see th18_diag_dump)
static unsigned int th18_bullets_in_play;
static unsigned int th18_bullets_moved;

// Whether the picture is held. A picture the game isn't updating (or isn't drawing) must not be
// moved forward either.
static bool th18_bullets_held() {
	void* pause = *th18_pause_object;
	if (pause != nullptr) {
		const unsigned int flags = *(unsigned int*)((char*)pause + th18_pause_flags);
		// What the game's two bullet tasks and the walk itself stop for: the update task for the
		// low bits, the draw task for bit 2, and the walk for bit 0x400 (which skips every
		// bullet's own update while the picture is held)
		if (((flags | (flags >> 2)) & 1) != 0 || (flags & 4) != 0 || (flags & 0x400) != 0)
			return true;
	}
	// The shop being open stops the update task as well
	return *th18_shop_object != nullptr;
}

// How much of a frame's movement a bullet in this state gets (FUN_00423E10, the switch on the state
// at +0xF68). A bullet that is in flight moves a whole step, the ones that are still waiting for
// their sub-pattern move half of one, and one that is in neither state doesn't move at all.
// 0x200 on a bullet holds it - the game neither moves nor draws such a bullet.
static float th18_bullet_step_factor(unsigned short state, unsigned int flags) {
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
// what it changed, so th18_bullets_undo can put it back.
static void th18_bullets_advance(float t) {
	th18_saved_bullets_count = 0;
	th18_bullets_in_play = 0;
	th18_bullets_moved = 0;

	void* manager = *th18_bullet_manager;
	if (manager == nullptr || th18_bullets_held())
		return;

	unsigned int frames_per_render = *th18_frames_per_render;
	if (frames_per_render > 2) // the game's own setting only goes to 2
		frames_per_render = 2;

	float scale = *th18_time_scale;
	if (!(scale > 0.0f) || scale > 2.0f)
		scale = 1.0f;

	const float step = t * (float)(frames_per_render + 1) * scale;

	void* node = *(void**)((char*)manager + th18_bullet_list_head);
	// The list is at most as long as the pool, and this runs with the game paused in its own frame
	// function, so it can't change under us; the bound is there to keep a list that got corrupted
	// some other way from running away with the frame.
	for (size_t guard = 0; node != nullptr && guard < th18_bullets_saved_max * 4; ++guard) {
		void* bullet = *(void**)node;
		void* next = *(void**)((char*)node + th18_bullet_list_next);
		if (bullet == nullptr)
			break;

		++th18_bullets_in_play;
		const float factor = th18_bullet_step_factor(
			*(unsigned short*)((char*)bullet + th18_bullet_state),
			*(unsigned int*)((char*)bullet + th18_bullet_flags));
		if (factor != 0.0f && th18_saved_bullets_count < th18_bullets_saved_max) {
			float* position = (float*)((char*)bullet + th18_bullet_position);
			const float* velocity = (const float*)((char*)bullet + th18_bullet_velocity);
			Th18SavedBullet& saved = th18_saved_bullets[th18_saved_bullets_count++];
			saved.bullet = bullet;

			const float bullet_step = step * factor;
			for (int axis = 0; axis < 3; ++axis) {
				saved.position[axis] = position[axis];
				position[axis] += velocity[axis] * bullet_step;
			}
			++th18_bullets_moved;
		}

		node = next;
	}
}

// Puts the positions of every bullet th18_bullets_advance moved back, so the game's own logic sees
// the state it left behind and not the projected one.
static void th18_bullets_undo() {
	for (size_t i = 0; i < th18_saved_bullets_count; ++i) {
		const Th18SavedBullet& saved = th18_saved_bullets[i];
		float* position = (float*)((char*)saved.bullet + th18_bullet_position);
		for (int axis = 0; axis < 3; ++axis)
			position[axis] = saved.position[axis];
	}
	th18_saved_bullets_count = 0;
}

// ---------------------------------------------------------------------------
// Frame interpolation, stage 1: the player, its shots and its ability cards' effects
// ---------------------------------------------------------------------------
//
// The player of the stage that is running is at 0x4CF410 (FUN_0045AF20 builds it - one 0x479D0
// byte object - and puts itself there, FUN_0045B020 releases it and clears the pointer). Its own
// picture is the animation object at +0x14, and the render pass of the player (0x45CAC0, the task
// the game registers with the draw chain - FUN_0045A7A0 registers it with the task id 0x1D, next
// to the update at 0x45CAA0 with the id 0x17) copies its position (+0x620, three floats) into that
// object's position (+0x604 = the object's +0x5F0) before drawing it, so moving the position moves
// the player's sprite the way the bullets' position moves theirs.
//
// Everything else the player puts on screen keeps a sprite object of its own and is positioned by
// the update instead, so those sprites have to be moved themselves:
//
//   - the shots the player fires are 128 slots of 0xF8 bytes at +0x1570 (FUN_0045E930 picks the
//     first one whose +0x8C is clear and walks them four at a time up to 0x200). A live slot keeps
//     the sprite object id it is drawn with at +0x08 and a position at +0x48, which the slot's own
//     update copies into that sprite (+0x5F0) before moving it. Anything a card adds to the
//     player's fire is a shot like any other and lands in these slots.
//   - the sprite the player's own shot is drawn with is kept at +0x476B0 (the movement code looks
//     it up and scales it every frame, and picks a new one out of the table at 0x405BF0 when the
//     field is empty).
//   - every ability card the player carries has the animation id of its in-game effect at +0x1C,
//     which the card's own code looks up as a sprite to put around the player (0x41051E and
//     0x410AD6 both push [card+0x1C] and call the lookup with it).
//
// The sprites those keep are found the way the game finds one from an id (0x488B40, ECX = the
// sprite manager [0x51F65C], the id on the stack): ids whose low 15 bits are 0x7FFF are on one of
// the manager's two lists, and every other id is a slot of 0x624 bytes starting at manager+0x700,
// with the "in use" byte at +0x61C and the id it belongs to at +0x544. The lookup is written out
// here instead of called: it takes its argument on the stack, and a call through a function
// pointer is not put together with that in mind - the garbage it gets back hands back a pointer
// that is not a sprite at all (the same mistake th15 made once).
//
// The player moves by what the input says rather than by a velocity that could be read back, and
// the shots and the card effects move by what comes out of that plus their own motion, so what
// each sprite moves by in a frame is measured instead of read: every game frame the positions are
// compared with the ones of the frame before, and the extra presentations move the sprites by that
// difference times the fraction of a frame they stand for. Something that didn't move - a held
// picture, a paused game - comes out as zero on its own.

static void** const th18_player = (void**)0x004CF410;
static const size_t th18_player_position = 0x620;      // the player's own position, 3 floats
static const size_t th18_player_shot_sprite = 0x476B0; // the sprite its own shot is drawn with
static const size_t th18_player_shots = 0x1570;        // 128 slots of 0xF8 bytes
static const size_t th18_player_shot_count = 128;
static const size_t th18_player_shot_size = 0xF8;
static const size_t th18_player_shot_live = 0x8C;      // nonzero while the shot is in use
static const size_t th18_player_shot_sprite_id = 0x08; // the sprite's object id
static void** const th18_ability_manager = (void**)0x004CF298;
static const size_t th18_ability_list = 0x1C;          // in it: the first card list node
static const size_t th18_ability_next = 0x04;          // in a node: the next one
static const size_t th18_card_effect_sprite = 0x1C;    // in a card: the id of its effect sprite
static const size_t th18_cards_max = 64;               // the most cards the player can carry
static void** const th18_sprite_objects = (void**)0x0051F65C;
static const size_t th18_object_slot_base = 0x700;     // in the manager: the slots by id
static const size_t th18_object_slot_size = 0x624;
static const size_t th18_object_in_use = 0x61C;        // in a slot: the "in use" byte
static const size_t th18_object_id = 0x544;            // in a slot: the id it belongs to
static const size_t th18_object_position = 0x5F0;      // in a sprite object, 3 floats
// Nothing on screen moves this far in one frame (the player's own speed is a few pixels), so a
// difference bigger than this is a slot that was handed to another object, not a movement
static const float th18_max_step = 48.0f;

// One thing to move for an extra presentation: the sprite object whose position is shifted. What
// the sprite moved by is measured on that same position, so the entry only has to remember it.
static const size_t th18_tracked_max = 1 + 1 + th18_player_shot_count + th18_cards_max;

struct Th18SpriteMotion {
	void* sprite;         // the sprite object it is about, null while the entry is free
	unsigned int frame;   // the frame it was last measured in
	float last_x, last_y;
	float delta_x, delta_y;
};

static Th18SpriteMotion th18_motions[th18_tracked_max];
static unsigned int th18_motion_frame = 1;
static float* th18_tracked[th18_tracked_max];
static size_t th18_tracked_count;
static float th18_tracked_saved[th18_tracked_max][3];

// Diagnostics: the largest movement the player's own position had since the last dump
static float th18_player_movement;
// Diagnostics: how many sprites this frame couldn't be given a movement record (see
// th18_sprite_motion - it should stay 0)
static unsigned int th18_sprites_untracked;

// The sprite object an id refers to: the same lookup the game does (0x488B40), written out instead
// of called (see the comment above).
static void* th18_object_by_id(unsigned int id) {
	const unsigned int index = id & 0x7FFF;
	if (id == 0 || index == 0x7FFF)
		return nullptr;

	void* manager = *th18_sprite_objects;
	if (manager == nullptr)
		return nullptr;

	char* object = (char*)manager + th18_object_slot_base + (size_t)index * th18_object_slot_size;
	if (*(unsigned char*)(object + th18_object_in_use) == 0)
		return nullptr;
	if (*(unsigned int*)(object + th18_object_id) != id)
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
static Th18SpriteMotion* th18_sprite_motion(void* sprite) {
	Th18SpriteMotion* empty = nullptr;
	Th18SpriteMotion* oldest = nullptr;
	for (size_t i = 0; i < th18_tracked_max; ++i) {
		Th18SpriteMotion& motion = th18_motions[i];
		if (motion.sprite == sprite)
			return &motion;
		if (motion.sprite == nullptr) {
			if (empty == nullptr)
				empty = &motion;
			continue;
		}
		if (motion.frame == th18_motion_frame)
			continue;
		if (oldest == nullptr || motion.frame < oldest->frame)
			oldest = &motion;
	}

	Th18SpriteMotion* motion = empty != nullptr ? empty : oldest;
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
static void th18_track(float* position) {
	if (th18_tracked_count >= th18_tracked_max)
		return;
	// Two ids can name the same sprite (the game doesn't mind), and moving one twice would put it
	// twice as far out and only be able to put back one of the two
	for (size_t i = 0; i < th18_tracked_count; ++i) {
		if (th18_tracked[i] == position)
			return;
	}
	Th18SpriteMotion* motion = th18_sprite_motion(position);
	if (motion == nullptr) {
		++th18_sprites_untracked;
		return;
	}

	const float x = position[0];
	const float y = position[1];

	// Only a sprite that was measured in the frame right before this one has something to compare
	// against (one that just appeared, or that came back after the game didn't draw for a while,
	// hasn't moved as far as this can tell)
	if (motion->frame + 1 == th18_motion_frame) {
		const float dx = x - motion->last_x;
		const float dy = y - motion->last_y;
		// The pools hand their slots out again, and the object that used to be there is somewhere
		// else entirely - that difference is not something to project
		if (dx > -th18_max_step && dx < th18_max_step && dy > -th18_max_step && dy < th18_max_step) {
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
	motion->frame = th18_motion_frame;
	motion->last_x = x;
	motion->last_y = y;

	th18_tracked[th18_tracked_count++] = position;
}

// Collects what is on screen this frame and works out what it moved by since the frame before.
// This runs once per game frame, from the game's own present (see th18_present_hook).
static void th18_player_measure() {
	th18_tracked_count = 0;
	th18_sprites_untracked = 0;
	++th18_motion_frame;

	void* player = *th18_player;
	if (player == nullptr)
		return;

	// The player itself: its own position is what the render pass reads
	th18_track((float*)((char*)player + th18_player_position));
	if (th18_tracked_count > 0) {
		const Th18SpriteMotion* motion = th18_sprite_motion(th18_tracked[0]);
		if (motion != nullptr) {
			const float x = motion->delta_x < 0.0f ? -motion->delta_x : motion->delta_x;
			const float y = motion->delta_y < 0.0f ? -motion->delta_y : motion->delta_y;
			const float magnitude = x > y ? x : y;
			if (magnitude > th18_player_movement)
				th18_player_movement = magnitude;
		}
	}

	// The sprite the player's own shot is drawn with: the update keeps it on the player
	{
		void* object = th18_object_by_id(*(unsigned int*)((char*)player + th18_player_shot_sprite));
		if (object != nullptr)
			th18_track((float*)((char*)object + th18_object_position));
	}

	// The shots, which are only measured while they are up
	for (size_t i = 0; i < th18_player_shot_count; ++i) {
		const char* slot = (const char*)player + th18_player_shots + i * th18_player_shot_size;
		if (*(const int*)(slot + th18_player_shot_live) == 0)
			continue;
		void* object = th18_object_by_id(*(const unsigned int*)(slot + th18_player_shot_sprite_id));
		if (object == nullptr)
			continue;
		th18_track((float*)((char*)object + th18_object_position));
	}

	// The effect sprite of every ability card the player carries, which the card's code puts
	// around the player (see the comment above)
	void* cards = *th18_ability_manager;
	if (cards != nullptr) {
		void* node = *(void**)((char*)cards + th18_ability_list);
		for (size_t guard = 0; node != nullptr && guard < th18_cards_max * 4; ++guard) {
			void* card = *(void**)node;
			if (card != nullptr) {
				void* object = th18_object_by_id(*(const unsigned int*)((char*)card + th18_card_effect_sprite));
				if (object != nullptr)
					th18_track((float*)((char*)object + th18_object_position));
			}
			node = *(void**)((char*)node + th18_ability_next);
		}
	}
}

// Moves the player, its shots and its cards' effect sprites t frames ahead of the state the game
// is in, and remembers what it changed
static void th18_player_advance(float t) {
	for (size_t i = 0; i < th18_tracked_count; ++i) {
		float* position = th18_tracked[i];
		for (int axis = 0; axis < 3; ++axis)
			th18_tracked_saved[i][axis] = position[axis];
		const Th18SpriteMotion* motion = th18_sprite_motion(position);
		if (motion != nullptr) {
			position[0] += motion->delta_x * t;
			position[1] += motion->delta_y * t;
		}
	}
}

static void th18_player_undo() {
	for (size_t i = 0; i < th18_tracked_count; ++i) {
		float* position = th18_tracked[i];
		for (int axis = 0; axis < 3; ++axis)
			position[axis] = th18_tracked_saved[i][axis];
	}
}

// ---------------------------------------------------------------------------
// Frame interpolation, stage 1: the straight lasers
// ---------------------------------------------------------------------------
//
// The lasers of a stage live in a manager of their own, which the stage setup builds right after
// the bullet manager (0x442C45: one 0x7C4 byte object, loading bullet.anm into +0x7B8) and puts in
// 0x4CF3F4. The manager threads them onto a list whose first laser is at manager+0x14 and whose
// next is the object's own +0x08; the update task (0x448870, thunk registered with the id 0x1C at
// 0x442CB3) walks it and calls each object's virtual update at vtable+0x10, and the draw task
// (0x4488E0, id 0x24) walks the same list calling vtable+0x14 and skipping the objects whose
// +0x10 says they are finished.
//
// The update task holds the picture the way the bullets' own tasks do, with the game thread object
// at 0x4CF2E4 (its flags at +0xB0): it stops for the low bits, for bit 0x400 and while the ability
// shop is open (0x4CF2A4), and for bit 2 it runs the walk with the global scale 0x4CCBF0 set to 0
// (the same trick th13, th14 and th15 use). The draw task stops for bit 2. All of those are the
// same "the picture is held" test the bullets use, so the lasers reuse it.
//
// The line laser's draw (0x44AB60) builds its picture out of three of its own fields: the position
// at +0x54, the angle at +0x6C and the length at +0x70. It copies the position into the first
// sprite it draws (at +0xBE8, whose position is +0x5F0 in) and puts the far end of the beam at
// position + polar(angle, length) (0x452C90), so moving those fields moves the laser - the same
// three fields th15, th16 and th17 have, at the same offsets.
//
// Each class of straight laser keeps what it moves by in a different place, so the layout is
// picked by vtable along with which classes are straight at all:
//
//   LaserLineInf (vtable 0x4B687C)      update 0x44A4F0: `pos += vel * [0x4CCBF0]` with the
//                                       velocity at 0x60/0x64/0x68 (0x44A6BE-0x44A6F2), the length
//                                       growing by the rate at 0x78 up to the clamp at 0x798
//                                       (0x44A66E-0x44A6A7), and it does not turn on its own.
//   LaserInfiniteInf (vtable 0x4B680C)  update 0x44CAE0: the velocity at 0x794/0x798/0x79C
//                                       (0x44CC32-0x44CC7B), the angle turning by the speed at
//                                       0x7A4 (0x44CB64-0x44CBCE) and the length growing by the
//                                       same 0x78 up to the clamp at 0x7A8 (0x44CB32-0x44CB5F).
//
// The beam class (vtable 0x4B679C) is deliberately not in the table: its own update (0x452B80) is
// a stub that just returns 0, so it never moves and there is nothing to project for it. The curve
// class (vtable 0x4B672C) is left out as the requirements ask.

// The laser manager of the stage that is running (null while no stage is)
static void** const th18_laser_manager = (void**)0x004CF3F4;
static const size_t th18_laser_list = 0x14;      // in it: the first laser
static const size_t th18_laser_next = 0x08;      // in a laser: the next one
static const size_t th18_laser_state = 0x10;     // in a laser: 1 once it is done, and not drawn
static const size_t th18_laser_position = 0x54;  // where it starts, 3 floats
static const size_t th18_laser_angle = 0x6C;     // the direction it points in, radians
static const size_t th18_laser_length = 0x70;    // how far it reaches

struct Th18LaserLayout {
	const void* vtable;
	size_t velocity;            // 3 floats
	size_t angle_speed;         // 0 for a class that doesn't turn
	size_t length_speed;
	size_t length_limit;
	size_t length_limit_subtract;
};

static const Th18LaserLayout th18_laser_layouts[] = {
	{ (const void*)0x004B687C, 0x60,  0,     0x78, 0x798, 0 },
	{ (const void*)0x004B680C, 0x794, 0x7A4, 0x78, 0x7A8, 0 },
};

static const size_t th18_lasers_max = 0x200 + 1; // the manager's own limit, plus its root entry

struct Th18LaserMoved {
	void* laser;
	float position[3];
	float angle, length;
};

static Th18LaserMoved th18_lasers_moved[th18_lasers_max];
static size_t th18_lasers_moved_count;
static unsigned int th18_lasers_seen;
static unsigned int th18_lasers_projected;

// The layout of the class this laser is, or null if it isn't one of the straight ones
static const Th18LaserLayout* th18_laser_layout(const void* laser) {
	const void* vtable = *(const void* const*)laser;
	for (size_t i = 0; i < sizeof(th18_laser_layouts) / sizeof(th18_laser_layouts[0]); ++i) {
		if (vtable == th18_laser_layouts[i].vtable)
			return &th18_laser_layouts[i];
	}
	return nullptr;
}

// Moves the straight lasers t frames ahead of the state the game is in, and remembers what it
// changed. Each laser is put where the velocity it carries right now would have it t frames from
// here: the game's own update adds velocity * the global scale to the position once per frame, and
// this is that same step for the fraction of a frame this presentation stands for.
static void th18_lasers_advance(float t) {
	th18_lasers_moved_count = 0;
	th18_lasers_seen = 0;
	th18_lasers_projected = 0;

	void* manager = *th18_laser_manager;
	if (manager == nullptr || th18_bullets_held())
		return;

	unsigned int frames_per_render = *th18_frames_per_render;
	if (frames_per_render > 2) // the game's own setting only goes to 2
		frames_per_render = 2;

	float scale = *th18_time_scale;
	if (!(scale > 0.0f) || scale > 2.0f)
		scale = 1.0f;

	const float step = t * (float)(frames_per_render + 1) * scale;

	void* laser = *(void**)((char*)manager + th18_laser_list);
	// The list is the game's own and this runs where nothing can change it; the bound is only so a
	// list that got broken some other way can't run away with the frame
	for (size_t guard = 0; laser != nullptr && guard < th18_lasers_max * 4; ++guard) {
		void* next = *(void**)((char*)laser + th18_laser_next);
		const Th18LaserLayout* layout = th18_laser_layout(laser);
		// A laser that is finished is not drawn, and the game takes it off the list on its next
		// update - nothing to project for it
		if (layout != nullptr && *(const int*)((char*)laser + th18_laser_state) != 1 &&
			th18_lasers_moved_count < th18_lasers_max) {
			++th18_lasers_seen;
			Th18LaserMoved& moved = th18_lasers_moved[th18_lasers_moved_count++];
			moved.laser = laser;

			float* position = (float*)((char*)laser + th18_laser_position);
			float* angle = (float*)((char*)laser + th18_laser_angle);
			float* length = (float*)((char*)laser + th18_laser_length);
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
				// the game only grows the length while it is short of its clamp
				if (limit > *length) {
					*length += *(const float*)((char*)laser + layout->length_speed) * step;
					if (*length > limit)
						*length = limit;
				}
			}
			++th18_lasers_projected;
		}
		laser = next;
	}
}

static void th18_lasers_undo() {
	for (size_t i = 0; i < th18_lasers_moved_count; ++i) {
		const Th18LaserMoved& moved = th18_lasers_moved[i];
		float* position = (float*)((char*)moved.laser + th18_laser_position);
		for (int axis = 0; axis < 3; ++axis)
			position[axis] = moved.position[axis];
		*(float*)((char*)moved.laser + th18_laser_angle) = moved.angle;
		*(float*)((char*)moved.laser + th18_laser_length) = moved.length;
	}
	th18_lasers_moved_count = 0;
}

// Diagnostics: press U to write the presentation timing of the last frames to oilp_diag.txt
static const unsigned int th18_diag_frames_max = 300;
static const unsigned int th18_diag_presents_max = 8;
struct Th18DiagFrame {
	LARGE_INTEGER base;                                      // the game's own present of the frame
	unsigned int presents;
	LARGE_INTEGER present_time[th18_diag_presents_max];      // when each presentation happened
};
static Th18DiagFrame th18_diag_frames[th18_diag_frames_max];
static unsigned int th18_diag_count;
static bool th18_diag_key_down;

static LARGE_INTEGER th18_perf_freq;

static void th18_diag_dump() {
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

	unsigned int count = th18_diag_count < th18_diag_frames_max ? th18_diag_count : th18_diag_frames_max;
	unsigned int start = th18_diag_count < th18_diag_frames_max ? 0 : th18_diag_count % th18_diag_frames_max;

	fprintf(file, "oilp frame interpolation diagnostics\n");
	fprintf(file, "logic frame rate: %.2f, configured presentation rate: %.2f\n",
		Limiter::game_fps, Limiter::PresentRate());
	fprintf(file, "frames recorded: %u\n", count);
	fprintf(file, "bullets: %u in play, %u projected forward (last game frame)\n",
		th18_bullets_in_play, th18_bullets_moved);
	fprintf(file, "player: %u sprite(s) moved (the player, its shots and its cards' effects)\n",
		(unsigned int)th18_tracked_count);
	fprintf(file, "player: %u sprite(s) this frame had no movement record (should stay 0)\n",
		th18_sprites_untracked);
	fprintf(file, "player movement: up to %.2f px per frame since the last dump\n",
		th18_player_movement);
	th18_player_movement = 0.0f;
	fprintf(file, "lasers: %u straight laser(s) in play (the curve ones are left alone)\n",
		th18_lasers_seen);

	double presents = 0.0;
	double first = 0.0;
	double last = 0.0;
	for (unsigned int i = 0; i < count; ++i) {
		const Th18DiagFrame& frame = th18_diag_frames[(start + i) % th18_diag_frames_max];
		fprintf(file, "frame %4u: %u present(s)", i, frame.presents);
		for (unsigned int p = 0; p < frame.presents && p < th18_diag_presents_max; ++p)
			fprintf(file, " [%u] %.2fms", p,
				(double)(frame.present_time[p].QuadPart - frame.base.QuadPart) * 1000.0 / (double)th18_perf_freq.QuadPart);
		fprintf(file, "\n");
		presents += frame.presents;
		if (i == 0)
			first = (double)frame.base.QuadPart;
		last = (double)frame.base.QuadPart;
	}
	if (last > first)
		fprintf(file, "average: %.3f presents per frame, %.1f presentations per second\n",
			presents / (double)count, presents * (double)th18_perf_freq.QuadPart / (last - first));

	fclose(file);
	printf("Wrote oilp_diag.txt\n");
}

// One extra presentation of the frame: the same render block the game runs, once more, so the state
// the game is in is drawn and presented again, with the drawn objects projected t frames ahead of
// it (stage 1) - t = 0 is the state the game's own presentation showed, and the presentations of
// one frame reach t = 1, which is where the game's next one takes over. The tool's frame is readied
// again, so the object render below draws its overlay into this presentation the same way it does
// in the game's own one.
static void th18_extra_presentation(float t) {
	IDirect3DDevice9* device = d3d9_hooked_device();
	if (device == nullptr)
		return;

	device->BeginScene();
	// The game's own pass set these before this point and the menus change them, so put back
	// whatever the scene was drawn with (see th18_render_hook)
	if (th18_scene_transforms) {
		device->SetTransform(D3DTS_VIEW, &th18_scene_view);
		device->SetTransform(D3DTS_PROJECTION, &th18_scene_projection);
	}
	if (th18_scene_viewport.Width != 0)
		device->SetViewport(&th18_scene_viewport);

	// The block itself, in the order the game runs it
	th18_reset_sprite_queue();
	*th18_render_flag = 0xFF;
	th18_draw_sprite_queue(*th18_sprite_manager);
	*th18_render_flag = 0;
	device->SetRenderState(D3DRS_FOGENABLE, 0);

	// The two floats that place the queue on screen have to be the ones the game's own pass left,
	// since the menus write their own and an extra presentation runs after the whole frame
	void* sprites = *th18_sprite_manager;
	if (sprites != nullptr) {
		*(float*)((char*)sprites + th18_sprite_queue) = th18_sprite_queue_saved[0];
		*(float*)((char*)sprites + th18_sprite_queue + 4) = th18_sprite_queue_saved[1];
	}

	ToolState::ReadyToDraw();
	// The objects are drawn where they will be t frames from now; the render bakes the positions
	// into the sprite queue, so they can go back right after it
	th18_bullets_advance(t);
	th18_player_advance(t);
	th18_lasers_advance(t);
	// The text list goes back to how the game's own run of the walk found it, so this presentation
	// draws the same text that one drew (see th18_text_record_count above)
	th18_text_records_restore();
	// The stage's draw callback advances its transition (see th18_stage_save); put the object back
	// afterwards so an extra pass cannot move the transition a second or third time
	th18_stage_save();
	th18_in_extra = true;
	// Put the shake back for this pass (see the comment above)
	extra_float_state_begin(&th18_shake);
	th18_render_objects();
	extra_float_state_end(&th18_shake);
	th18_in_extra = false;
	th18_stage_restore();
	th18_lasers_undo();
	th18_player_undo();
	th18_bullets_undo();
	th18_draw_sprite_queue(*th18_sprite_manager);
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
static HRESULT th18_present_hook() {
	IDirect3DDevice9* device = d3d9_hooked_device();
	if (device == nullptr)
		return E_FAIL;

	static bool freq_queried = false;
	if (!freq_queried) {
		freq_queried = true;
		QueryPerformanceFrequency(&th18_perf_freq);
	}

	LARGE_INTEGER base;
	QueryPerformanceCounter(&base);

	// The game's own update has run and it is about to put that frame on screen: this is the one
	// point per frame where the movement of the player, its shots and its cards' effects can be
	// measured
	th18_player_measure();

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
	LARGE_INTEGER present_time[th18_diag_presents_max];
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

				if (presents < th18_diag_presents_max)
					QueryPerformanceCounter(&present_time[presents]);
				++presents;

				// Where this presentation sits between the game's own one and the next one:
				// the frames of one game frame are spread evenly over it, so the k-th of
				// extras + 1 of them draws the state k/(extras + 1) frames along (stage 1)
				th18_extra_presentation((float)k / (float)(extras + 1));
			}
		}
	}

	// Record the frame for the diagnostics
	{
		Th18DiagFrame& frame = th18_diag_frames[th18_diag_count % th18_diag_frames_max];
		frame.base = base;
		frame.presents = presents;
		for (unsigned int p = 0; p < th18_diag_presents_max; ++p)
			frame.present_time[p] = p < presents ? present_time[p] : base;
		++th18_diag_count;
	}

	// U dumps the diagnostics
	if (GetAsyncKeyState('U') & 0x8000) {
		if (!th18_diag_key_down) {
			th18_diag_key_down = true;
			th18_diag_dump();
		}
	} else {
		th18_diag_key_down = false;
	}

	return result;
}

// The present the game sets up at each present call site: the arguments are pushed and the device
// is called, 14 bytes in all (see th18_install_patches)
static const size_t th18_present_setup_size = 14;

// Jumps over the present the game sets up after its present call site, so the code after it runs
// with the result the hook left in eax
static void th18_skip_present_setup(DWORD address) {
	BYTE patch[th18_present_setup_size];
	patch[0] = 0xE9;
	*(DWORD*)(patch + 1) = (address + th18_present_setup_size) - (address + 5);
	for (size_t i = 5; i < th18_present_setup_size; ++i)
		patch[i] = 0x90;
	patch_bytes(address, patch, sizeof(patch));
}

void th18_install_patches() {
	{
		// Skip the original frame limiter
		BYTE patch[] = { 0xEB, 0x53 };
		patch_bytes(0x00472E25, patch, sizeof(patch));
	}
	{
		// Force fast input latency mode
		BYTE patch[] = { 0xE9, 0xB2, 0x01, 0x00, 0x00, 0x90 };
		patch_bytes(0x00471A9E, patch, sizeof(patch));
	}
	{
		// Hook window update
		patch_call(0x00471C5A, th18_window_update_hook);
	}
	if (Config::D3D9Ex) {
		// Redirect Direct3DCreate9 call
		// IAT is being hooked, but thcrap also hooks Direct3DCreate9 in the same place
		// This should force our Direct3DCreate9 hook to be loaded no matter what
		patch_call(0x00471634, Direct3DCreate9_hook);

		// Extra NOP is needed because we're replacing a FF 15 call, which is 6 bytes long
		BYTE patch[] = { 0x90 };
		patch_bytes(0x00471639, patch, sizeof(patch));
	}
	if (Config::ReplaySpeedControl) {
		// Skip the original replay speed control stuff
		BYTE patch[] = { 0x90, 0x90 };
		patch_bytes(0x00461DD3, patch, sizeof(patch));
	}
	if (Config::Interpolation || Config::InterpolationFPS) {
		// Take over the object render, only to remember the viewport, the transforms and the two
		// queue floats the scene is drawn with (see th18_render_hook); the render itself runs
		// exactly the way it always did
		static const DWORD render_calls[] = { 0x00471BCB, 0x00472F21, 0x0047307A };
		unsigned int render_patched = 0;
		for (DWORD address : render_calls) {
			BYTE* render_call = (BYTE*)address;
			if (render_call[0] == 0xE8) {
				patch_call(address, (void*)th18_render_hook);
				++render_patched;
			} else {
				printf("th18 render call at 0x%x doesn't look like a known one, skipping!\n", address);
			}
		}

		// The fade creation block does not check its allocation for null, so its guard branch is
		// trampolined instead (see th18_fade_create_branch)
		BYTE* fade_branch = (BYTE*)0x0041C488;
		if (fade_branch[0] == 0x0F && fade_branch[1] == 0x8D &&
			(DWORD)0x0041C48E + *(DWORD*)(fade_branch + 2) == 0x0041C5A3) {
			patch_jmp(0x0041C488, (void*)th18_fade_create_branch);
			BYTE nop = 0x90;
			patch_bytes(0x0041C48D, &nop, 1);
		} else {
			printf("th18 fade branch at 0x41C488 doesn't look like the known one, skipping!\n");
		}

		// Take over the presents in the three frame functions. The instruction that loads the
		// device for the present is 5 bytes, which is exactly what the hook's call needs, and the
		// 14 bytes after it (the arguments and the present call itself) are skipped over - the hook
		// presents with the same device and arguments the game would have used.
		static const DWORD present_sites[] = { 0x00472D1D, 0x00472F5C, 0x00473129 };
		unsigned int present_patched = 0;
		for (DWORD address : present_sites) {
			BYTE* present_site = (BYTE*)address;
			if (present_site[0] == 0xA1 && *(DWORD*)(present_site + 1) == 0x004CCDF8) {
				patch_call(address, (void*)th18_present_hook);
				th18_skip_present_setup(address + 5);
				++present_patched;
			} else {
				printf("th18 present call at 0x%x doesn't look like a known one, skipping!\n", address);
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

FPSTarget th18_replay_callback() {
	if (*CReplayManager::InstancePtr && (*CReplayManager::InstancePtr)->mode == 1) {
		// TODO: Reverse engineer this struct instead of being lazy
		auto input = *(DWORD*)0x004CA210;
		if (input & InputState::Focus)
			return FPSTarget::ReplaySlow;
		else if (input & InputState::Shoot) // LCTRL isn't read at all in this game
			return FPSTarget::ReplaySkip;
	}
	return FPSTarget::Game;
}
