#pragma once

#include <Windows.h>
#include "games.h"

// Frame limiter based on vpatch
class Limiter {
public:
	static void Initialize(ReplayCallback replay_callback);
	static void Tick();

	// Ticks the limiter, waiting for the given amount of time instead of the limiter's own frame
	// schedule. The games that keep a clock of their own (th19, th20) hand it the time their
	// clock says is left until the next frame, so the wait ends exactly when the game wants to
	// run it: the limiter only makes the game's own pacing precise instead of fighting it with a
	// schedule that can drift out of phase with the game's clock.
	static void TickUntil(double seconds);

	static bool SetGameFPS(int fps);

	// Whether this frame's present should actually be submitted.
	// True unless the current target framerate is above the display refresh rate,
	// in which case we present every Nth frame so the present can't throttle the logic.
	static bool ShouldPresent();

	// Some games (th19) keep their own clock and compute how long a frame is from the frame
	// rate. Those games read this value directly, so it has to follow the limiter's target -
	// otherwise the game's clock keeps running at 60fps no matter what GameFPS is set to.
	// See touhou19.cpp for the games that use it.
	static double game_fps;

	// Those same games are the ones external tools hook to change the frame rate: thprac, for
	// example, points the engine's clock at a variable of its own. The per game code reports
	// whatever frame rate is in use there through this, so the limiter follows it instead of
	// fighting over the engine's clock. Returns 0 when there's nothing to follow.
	static UINT (*external_game_fps)();

private:
	static bool UpdateTargetFPS();

	static bool initialized;
	static LARGE_INTEGER start_time;
	static unsigned int frame_num;
	static LARGE_INTEGER wait_amount;
	static LARGE_INTEGER last_wait_amount;
	static LARGE_INTEGER blt_prepare_time;
	static LARGE_INTEGER perf_freq;
	static ReplayCallback replay_callback;
	static LARGE_INTEGER frame_start;
	static LARGE_INTEGER frame_end;
	static unsigned int present_every; // 1 = present every frame
	static unsigned int display_refresh;
	static bool refresh_queried;
};
