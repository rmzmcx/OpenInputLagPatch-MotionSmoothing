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
	// What the patch's overlay shows, as a sum of the two readouts it has:
	//   0  nothing (no overlay at all)
	//   1  the rate the frames are actually presented at ("144.0 fps"), which is the default
	//   2  the render time line ("0.65/2.00ms": how long the game's own part of the frame took, and
	//      what BltPrepareTime is set to - this is the line that setting is tuned with)
	//   3  both
	static int ShowOverlay;
	static BOOL DebugConsole;
	static BOOL DebugWait;
	static BOOL FixInputGlitching;

	// Whether the patch should replace the frame rate the game measures for itself, the way vpatch's
	// CalcFPS=1 does. Touhou 6, 7 and 8 count their own draw passes and work their frame rate out of
	// them over half second windows, and the ratio of that to half a second of frames is the
	// "processing rate" the results screen shows and a replay records (0.00% is a run that never
	// fell behind). A window that a load or a scene change lands in reads as a slow one, and the run
	// carries that as lost frame rate from then on, which is why a run that spends any time between
	// stages shows a few percent where a run that doesn't shows none. vpatch's CalcFPS sidesteps
	// that measurement altogether; this does the same thing with the frame rate the limiter measures
	// for itself (see Limiter::CountGameFrame). vpatch's default is 1, so that is the default here.
	static BOOL CalcFPS;

	// Frame interpolation (motion smoothing). The value is read as text because it has more than
	// one form:
	//   0   = off
	//   1   = present at the display's own refresh rate, i.e. as many presentations a second as the
	//         display can show (the default; on a 144hz display 60fps logic is presented at 144)
	//   -1  = pick the largest whole multiple of the game's frame rate the display can show, which
	//         keeps the presentations evenly spaced (on a 144hz display 60fps logic gives 120)
	//   N   = present N times per game frame
	//   *R  = present at R frames per second no matter what the game's frame rate is (e.g. *144)
	// Interpolation holds the first four forms and InterpolationFPS holds the last one, so
	// InterpolationFPS takes priority when it is set.
	static int Interpolation;
	static UINT InterpolationFPS;

	// Which parts the interpolation moves in the extra presentations. The master switch above must
	// also be on; these only choose between the things it can move.
	static BOOL InterpolatePlayerBody;
	static BOOL InterpolatePlayerShots;
	static BOOL InterpolateBulletsAndLasers;

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
