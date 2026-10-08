#pragma once

#include <Windows.h>
#include "games.h"

// Frame limiter based on vpatch
class Limiter {
public:
	static void Initialize(ReplayCallback replay_callback);
	static void Tick();

	static bool SetGameFPS(int fps);

	// Whether this frame's present should actually be submitted.
	// True unless the current target framerate is above the display refresh rate,
	// in which case we present every Nth frame so the present can't throttle the logic.
	static bool ShouldPresent();

	// Some games (th19) don't have a usable per-frame hook to tick from. Those get ticked
	// from the D3D9 EndScene hook instead, which runs exactly once per rendered frame - right
	// where the game would have run its own frame limiter.
	static bool tick_on_end_scene;

	// Some games (th19) keep their own clock and compute how long a frame is from the frame
	// rate. Those games read this value directly, so it has to follow the limiter's target -
	// otherwise the game's clock keeps running at 60fps no matter what GameFPS is set to.
	// See touhou19.cpp for the games that use it.
	static double game_fps;

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
