#pragma once

// BOOL is used instead of bool just in case someone tries to load a non-bool value into something that's supposed to be a bool
#include <Windows.h>
#include "games.h"

enum TargetRefreshRate {
	Max,
	Sixty,
	MultipleOfSixty,
};

enum SleepType {
	Spin,
	Vpatch,
};

// See the README for documentation on these values
class Config {
public:
	static bool Load();

	static UINT GameFPS;
	static BOOL ReplaySpeedControl;
	static UINT ReplaySkipFPS;
	static UINT ReplaySlowFPS;
	static UINT BltPrepareTime;
	static SleepType Sleep;
	static BOOL D3D9Ex;
	static TargetRefreshRate FullscreenRefreshRate;
	static BOOL ShowOverlay;
	static BOOL DebugConsole;
	static BOOL DebugWait;
	static BOOL FixInputGlitching;

	// Frame interpolation (motion smoothing). The value is read as text because it has more than
	// one form:
	//   0   = off
	//   -1  = pick the largest multiple of the game's frame rate the display can show
	//   N   = present N times per game frame
	//   *R  = present at R frames per second no matter what the game's frame rate is (e.g. *144)
	// Interpolation holds the first three forms and InterpolationFPS holds the fourth one, so
	// InterpolationFPS takes priority when it is set.
	static int Interpolation;
	static UINT InterpolationFPS;

	// Keep rendering (and presenting) while the game window isn't active, so the picture
	// doesn't freeze after alt-tabbing. Equivalent to vpatch's AlwaysBlt.
	// Only implemented for th06 and th07.
	static BOOL AlwaysBlt;
	static TouhouGame GameOverride;

	// [Window] section - same idea as vpatch's window mode support
	// Ask whether the game should run in fullscreen mode on boot, so the game's own
	// fullscreen setting doesn't have to be edited
	static BOOL AskWindowMode;
	// Whether the settings below should be applied while the game runs in window mode
	static BOOL WindowEnabled;
	// Position and size of the game's window, in pixels. CW_USEDEFAULT (0x80000000) lets
	// Windows pick the position, and a width or height of 0 keeps the game's own size
	static UINT WindowX;
	static UINT WindowY;
	static UINT WindowWidth;
	static UINT WindowHeight;
	// Whether the window keeps its title bar, and whether it's always on top
	static BOOL WindowTitleBar;
	static BOOL WindowAlwaysOnTop;
	// Keeps the taskbar out of the way while the game's window is the active one, without
	// holding the window above every other one the way WindowAlwaysOnTop does. 1 marks the
	// window as a fullscreen one for the shell, 2 holds it on top only while it's active
	// (see window_mode.cpp for what the two ways of doing that are)
	static int WindowCoverTaskbar;
};
