#pragma once

#include <Windows.h>

// Window mode support, matching vpatch's [Window] section in vpatch.ini.
//
// Like vpatch, this makes the game itself switch between fullscreen and window mode, because
// the games read their own "windowed" flag before creating the window. Forcing only the D3D
// presentation parameters is not enough: with dgVoodoo2 set up to turn fullscreen into a
// borderless window (FullscreenAttributes = fake, AppControlledScreenMode = false) the game
// keeps looking and behaving like a windowed one, because the window it created is a windowed
// one. vpatch solves this by overwriting the flag in the game's own config before the game
// uses it; that's what happens here too.
//
// Only the games that don't have an in-game option for this need it (Touhou 6 to 9.5); later
// games have their own screen mode setting, so they're left alone. The window settings below
// (position, size, title bar, always on top) apply to every supported game though.
namespace WindowMode {
	// Installs the hooks the window mode question relies on. Call this from the patcher
	// entry point, before the game's own code runs.
	void Hook();

	// Mode requested by the window mode question on boot, if any
	enum class Override {
		None,
		Windowed,
		Fullscreen,
	};
	// What was picked on boot, if anything
	Override GetOverride();

	// Same as GetOverride, but stops reporting an override once the game's own setting no
	// longer matches it (the mode was changed from inside the game), so the presentation
	// parameters aren't forced back to the boot time choice
	Override GetEffectiveOverride();

	// Asks the window mode question if it hasn't been asked yet. This is the fallback for
	// games whose single instance mutex isn't seen by the mutex hook; normally the question
	// is asked from there, because that's the earliest point where the mutex (which thprac
	// and other tools detect the game by) exists.
	void AskIfNeeded();

	// Called right before the game creates its Direct3D interface, which is after it read
	// its config but before it creates its window. Overwrites the game's own windowed flag
	// with the mode that was picked on boot.
	void OnD3DCreate();

	// Applies the [Window] settings to the window the game renders into. Call this once the
	// device has been created or reset; `windowed` is the mode the device ended up in.
	void Apply(HWND hwnd, BOOL windowed);
}
