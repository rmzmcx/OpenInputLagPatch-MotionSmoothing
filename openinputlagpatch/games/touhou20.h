#pragma once
// Touhou 20: Fossilized Wonders v1.00c

#include <Windows.h>

namespace Touhou20 {
	// Bits in the game's own input state
	enum InputState {
		Shoot = 1,
		Focus = 8,
	};

	// th20 runs on the same engine as th19, so it gets the same treatment: the game's frame
	// limiter (0x004193E0 in the disassembly) is disabled and the patch's own limiter takes over
	// from the D3D9 EndScene hook, and the frame rate the engine's own clock is built on is
	// pointed at GameFPS - without that, the game keeps running at its original speed no matter
	// what the frame limiter is set to. See touhou20.cpp for the details.
	//
	// th20 has a replay system. The game runs replay playback as its own app state (the same
	// variable the rest of the game uses to tell its tasks apart), which is what the replay
	// callback below checks before it lets the replay keys change the framerate - holding the
	// shoot key fast forwards, holding the slow key slows down, like on th17 and th18.
	//
	// Like th19, th20 is built with ASLR enabled (DYNAMIC_BASE with relocations), so every
	// address has to be rebased onto wherever the executable got loaded - see rebase() in
	// touhou20.cpp.
}
