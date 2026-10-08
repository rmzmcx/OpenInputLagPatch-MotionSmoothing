#pragma once
// Touhou 19: Unfinished Dream of All Living Ghost v1.10c

#include <Windows.h>

namespace Touhou19 {
	// The game's frame limiter (0x004B6240 in the disassembly) is called at the end of every
	// frame, right after EndScene and before the timing bookkeeping. OpenInputLagPatch replaces
	// that call with its own limiter so the game gets paced by the custom frame limiter instead.
	// th19 has no replay system, so there is no replay callback for it.
	//
	// The engine also keeps a clock of its own, and computes how long a frame is from a
	// hardcoded 60fps that's baked into several instructions - so on top of replacing the frame
	// limiter, those have to be pointed at GameFPS as well, or the game keeps running at its
	// original speed no matter what the limiter does. See touhou19.cpp for the details.
	//
	// Unlike the older games, th19 is built with ASLR enabled (DYNAMIC_BASE with relocations),
	// so every address has to be rebased onto wherever the executable got loaded - see
	// rebase() in touhou19.cpp.
}
