// Touhou 19: Unfinished Dream of All Living Ghost v1.10c

#include <stdio.h>
#include "patch_util.h"
#include "limiter.h"
#include "d3d9_hook.h"
#include "config.h"
#include "touhou19.h"

using namespace Touhou19;

// th19 is built with ASLR enabled, unlike the older games, so the addresses from the
// disassembly (which all assume the default 0x400000 image base) have to be rebased on the
// base the executable actually got loaded at
static DWORD rebase(DWORD address) {
	static DWORD image_base = (DWORD)GetModuleHandleW(nullptr);
	return image_base + (address - 0x400000);
}

// Unlike the older games, th19 doesn't just run its logic once per rendered frame: each update
// function advances the engine's clock by one frame at a time and only runs the game's logic
// (and renders) once that clock says a frame has passed. How long a frame is comes from a
// hardcoded double holding the frame rate (0x005AA2C0 is 60.0), which is read as
// 1.0 / frame_rate wherever it's needed - that's why the game kept running at 60fps no matter
// what the frame limiter was set to.
//
// Pointing those reads at the limiter's frame rate instead moves the engine's clock (and with
// it the game's speed) to GameFPS, the same thing thprac's FPS option does for th11 and up.
// Each address below is the 4 byte displacement of a MOVSD/DIVSD that reads 0x005AA2C0, so the
// patch only changes which double the instruction reads.
//
// The game's own frame limiter (0x004B6240) reads the same constant, but it's disabled below,
// so it doesn't have to be patched.
static const DWORD fps_references[] = {
	0x004B5C9B, // Sleep based pacer of the update path at 0x004B5C70
	0x004B5E8E, // Sleep based pacer of the update path at 0x004B5D70
	0x004B6090, // frame stepping of the update path at 0x004B5FE0
	0x004B650D, // frame stepping of the update path at 0x004B64E0
	0x004B7639, // frame stepping of the main loop's own catch up path
};

static void patch_fps_reference(DWORD address) {
	DWORD location = rebase(address);

	// Only patch if the instruction still reads the engine's frame rate: an unknown build
	// shouldn't get random data written into the middle of its code. The address in the
	// instruction is an absolute one, so it gets relocated along with the executable and has
	// to be rebased before it can be compared
	DWORD frame_rate = rebase(0x005AA2C0);
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
double th19_frame_wait_ms = 0.0;
void th19_frame_wait_impl();

void __declspec(naked) th19_frame_wait() {
	__asm {
		movsd qword ptr [th19_frame_wait_ms], xmm0
		jmp th19_frame_wait_impl
	}
}

void th19_frame_wait_impl() {
	Limiter::TickUntil(th19_frame_wait_ms / 1000.0);
}

static void patch_frame_wait(DWORD address) {
	BYTE* sequence = (BYTE*)rebase(address);

	// push 1 / call dword ptr [Sleep]
	if (sequence[0] != 0x6A || sequence[1] != 0x01 || sequence[2] != 0xFF || sequence[3] != 0x15) {
		printf("Frame wait at 0x%x doesn't look like a known one, skipping!\n", address);
		return;
	}

	BYTE patch[8] = { 0xE8, 0x00, 0x00, 0x00, 0x00, 0x90, 0x90, 0x90 };
	*(DWORD*)(patch + 1) = (DWORD)&th19_frame_wait - ((DWORD)sequence + 5);
	patch_bytes(sequence, patch, sizeof(patch));
}

// thprac changes the frame rate of games like this one by pointing the engine's clock at a
// variable of its own (0x004B6090, the spot the update path the game uses reads). If that
// happens the game and the limiter would disagree about how long a frame is, so the operand is
// taken back - but the value the other tool put there is followed, since tools like thprac are
// what the player configures in game and their frame rate wins over the configured GameFPS.
static double* external_fps = nullptr;

static UINT th19_external_game_fps() {
	DWORD* operand = (DWORD*)rebase(0x004B6090);

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

void th19_install_patches() {
	{
		// Make the game's frame limiter return immediately. It is called from more than one
		// code path, and which one the game actually uses is hard to pin down, so disabling
		// the function itself covers all of them. Our limiter takes over its job.
		BYTE patch[] = { 0xC3 };
		patch_bytes(rebase(0x004B6240), patch, sizeof(patch));
	}
	{
		// The engine starts reading its frame rate long before the limiter is ticked for the
		// first time, so start it off with the configured value
		Limiter::game_fps = (double)Config::GameFPS;

		// Follow thprac (or any other tool) if it takes the engine's frame rate over
		Limiter::external_game_fps = th19_external_game_fps;
	}
	{
		// Run the engine's clock at GameFPS instead of its hardcoded 60
		for (DWORD reference : fps_references)
			patch_fps_reference(reference);

		// Let the limiter do the waiting for the frames instead of the game's own Sleep(1)
		patch_frame_wait(0x004B605C);
	}
	if (Config::D3D9Ex) {
		// Redirect the Direct3DCreate9 call
		// IAT is being hooked, but thcrap also hooks Direct3DCreate9 in the same place
		// This should force our Direct3DCreate9 hook to be loaded no matter what
		patch_call(rebase(0x004B71A2), Direct3DCreate9_hook);

		// Extra NOP is needed because we're replacing a FF 15 call, which is 6 bytes long
		BYTE patch[] = { 0x90 };
		patch_bytes(rebase(0x004B71A7), patch, sizeof(patch));
	}
}
