// Touhou 20: Fossilized Wonders v1.00c

#include <stdio.h>
#include "patch_util.h"
#include "limiter.h"
#include "d3d9_hook.h"
#include "config.h"
#include "touhou20.h"

using namespace Touhou20;

// th20 is built with ASLR enabled, like th19, so the addresses from the disassembly (which all
// assume the default 0x400000 image base) have to be rebased onto the base the executable
// actually got loaded at
static DWORD rebase(DWORD address) {
	static DWORD image_base = (DWORD)GetModuleHandleW(nullptr);
	return image_base + (address - 0x400000);
}

// Like th19, th20 doesn't run its logic once per rendered frame: each update function advances
// the engine's own clock by one frame at a time and only runs the game's logic once that clock
// says a frame has passed. How long a frame is comes from a hardcoded 60fps (0x0056C470 holds
// 60.0), which is read as 1.0 / frame_rate, so disabling the frame limiter alone leaves the
// game running at its original speed no matter what GameFPS is set to.
//
// The addresses below are the 4 byte displacements of the MOVSD/DIVSD instructions that read
// 0x0056C470, so patching them only changes which double the instruction reads. thprac does the
// same thing for th20 (it patches 0x00419EAF, the one in the update path the game actually
// uses).
//
// The game's own frame limiter (0x004193E0) reads the constant as well, but it's disabled
// below, so it doesn't have to be patched.
static const DWORD fps_references[] = {
	0x00419A9C, // Sleep based pacer of the update path at 0x00419A50
	0x00419AD2, // the same pacer, second use of the constant
	0x00419C5A, // frame stepping of the update path at 0x00419C20
	0x00419EAF, // frame stepping of the update path at 0x00419DE0
};

static void patch_fps_reference(DWORD address) {
	DWORD location = rebase(address);

	// Only patch if the instruction still reads the engine's frame rate: an unknown build
	// shouldn't get random data written into the middle of its code. The address in the
	// instruction is an absolute one, so it gets relocated along with the executable and has
	// to be rebased before it can be compared
	DWORD frame_rate = rebase(0x0056C470);
	if (*(DWORD*)location != frame_rate) {
		printf("Frame rate reference at 0x%x doesn't look like a known one (found 0x%x), skipping!\n",
			address, *(DWORD*)location);
		return;
	}

	DWORD value = (DWORD)&Limiter::game_fps;
	patch_bytes(location, &value, sizeof(value));
}

// The game paces its frames itself: once its own clock is more than 1.5ms ahead of the current
// time, it sleeps a single millisecond before it gets any work done. That wakeup is far too
// imprecise to pace a frame with (Sleep(1) rarely returns after exactly one millisecond, so the
// frames end up a millisecond or two out of place), so the call is redirected into the patch's
// limiter instead.
//
// The wait uses exactly what the game itself worked out: right before it calls Sleep, the code
// has the time left until its clock says the next frame is due, multiplied by 1000, sitting in
// XMM0 (that's the "more than 1.5ms" it compared just before). Waiting for that means the limiter
// ends up in phase with the game's clock by construction - no schedule of its own that could
// drift - and the game's clock is the thing that decides how fast the game runs anyway.
//
// This replaces a "push 1 / call Sleep", so the argument isn't pushed anymore and the hook takes
// none.
double th20_frame_wait_ms = 0.0;
void th20_frame_wait_impl();

void __declspec(naked) th20_frame_wait() {
	__asm {
		movsd qword ptr [th20_frame_wait_ms], xmm0
		jmp th20_frame_wait_impl
	}
}

void th20_frame_wait_impl() {
	Limiter::TickUntil(th20_frame_wait_ms / 1000.0);
}

static void patch_frame_wait(DWORD address) {
	BYTE* sequence = (BYTE*)rebase(address);

	// push 1 / call dword ptr [Sleep]
	if (sequence[0] != 0x6A || sequence[1] != 0x01 || sequence[2] != 0xFF || sequence[3] != 0x15) {
		printf("Frame wait at 0x%x doesn't look like a known one, skipping!\n", address);
		return;
	}

	BYTE patch[8] = { 0xE8, 0x00, 0x00, 0x00, 0x00, 0x90, 0x90, 0x90 };
	*(DWORD*)(patch + 1) = (DWORD)&th20_frame_wait - ((DWORD)sequence + 5);
	patch_bytes(sequence, patch, sizeof(patch));
}

// thprac changes the frame rate of games like this one by pointing the engine's clock at a
// variable of its own. If that happens, the game and the limiter would disagree about how long
// a frame is - and the replay speed control would lose the clock it drives. So the operand is
// taken back, but the value the other tool put there is followed: tools like thprac are what
// the player configures in game, so their frame rate wins over the configured GameFPS.
static double* external_fps = nullptr;

static UINT th20_external_game_fps() {
	DWORD* operand = (DWORD*)rebase(0x00419EAF);

	if (*operand != (DWORD)&Limiter::game_fps) {
		double* candidate = (double*)*operand;
		if (*candidate > 1.0 && *candidate < 10000.0)
			external_fps = candidate;

		DWORD value = (DWORD)&Limiter::game_fps;
		patch_bytes(operand, &value, sizeof(value));
	}

	if (external_fps == nullptr)
		return 0;

	// thprac writes this whenever its framerate option is applied, so it has to be read back
	// every time instead of being copied once
	double rate = *external_fps;
	if (rate < 1.0 || rate > 10000.0)
		return 0;
	return (UINT)(rate + 0.5);
}

void th20_install_patches() {
	{
		// Make the game's frame limiter return immediately, exactly like on th19 - it's called
		// from the update paths that render, and our limiter (ticked from the EndScene hook)
		// takes over its job
		BYTE patch[] = { 0xC3 };
		patch_bytes(rebase(0x004193E0), patch, sizeof(patch));
	}
	{
		// The engine starts reading its frame rate long before the limiter is ticked for the
		// first time, so start it off with the configured value
		Limiter::game_fps = (double)Config::GameFPS;

		// Follow thprac (or any other tool) if it takes the engine's frame rate over
		Limiter::external_game_fps = th20_external_game_fps;
	}
	{
		// Run the engine's clock at GameFPS instead of its hardcoded 60
		for (DWORD reference : fps_references)
			patch_fps_reference(reference);

		// Let the limiter do the waiting for the frames instead of the game's own Sleep(1)
		patch_frame_wait(0x00419E66);
	}
	if (Config::D3D9Ex) {
		// Redirect the Direct3DCreate9 call (0x0041C335 calls the d3d9.dll import thunk).
		// The IAT is being hooked as well, but thcrap hooks Direct3DCreate9 in the same place,
		// so this makes sure our hook is the one that gets called
		patch_call(rebase(0x0041C335), Direct3DCreate9_hook);
	}
}

// The game runs replay playback as its own app state, which is what this checks before it lets
// the replay keys change the framerate: a replay playing back is state 2 with the replay file
// object loaded, the other states are the menus, a normal game and so on.
static const int replay_state = 2;

FPSTarget th20_replay_callback() {
	if (*(int*)rebase(0x005C6128) != replay_state || *(DWORD*)rebase(0x005C60FC) == 0)
		return FPSTarget::Game;

	// While a replay plays back, the input state holds the recorded inputs, which is exactly
	// what the game's own fast forward and slow down go by: the shoot key skips ahead (like on
	// th17 and th18, holding ctrl does nothing in these games) and the focus key slows down
	auto input = *(DWORD*)rebase(0x005B88B4);
	if (input & InputState::Focus)
		return FPSTarget::ReplaySlow;
	else if (input & InputState::Shoot)
		return FPSTarget::ReplaySkip;
	return FPSTarget::Game;
}
