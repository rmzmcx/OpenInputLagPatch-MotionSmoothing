#pragma once

#include <Windows.h>
#include "fps_target.h"

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

	// The frame rate the games that measure their own (Touhou 6, 7 and 8) are told about, for
	// Config::CalcFPS (see the comment on it): those games add one to a counter every frame and work
	// their frame rate out of it over half second windows, and that reading is the "processing rate"
	// their results screen shows and a replay records. With CalcFPS on the counter the games add to
	// is replaced along with their fps hook, and:
	//   - CountGameFrame is called once per frame of the game, from that hook, so what is measured
	//     here is the frame rate the game is actually being run at
	//   - MeasuredFps is that measurement, in frames per second, and is what the game is handed in
	//     place of the reading it would have worked out for itself
	// It is measured over the same half second windows the games use, so the value follows the real
	// frame rate the way their own reading would have, without a load or a scene change counting as
	// lost frame rate for the rest of the run.
	static void CountGameFrame();
	static float MeasuredFps();

#ifndef OILP_X64
	// Waits until the given point in time, in performance counter units. The frame interpolation
	// uses this to place the extra presentations inside a frame.
	static void WaitUntil(LARGE_INTEGER target);
#endif

	// The games with frame interpolation present more than once per frame. The extra presentations
	// are the patch's own work and shouldn't count towards the frame time the overlay reports, so
	// the interpolation code calls this where the game's own frame ended.
	static void MarkFrameWorkEnd();

	// Re-reads the current target frame rate, including an external tool's value and the replay
	// callback.
	//
	// Tick() and TickUntil() do this on every frame they pace, but a game that paces itself
	// through TickUntil only reaches it while its own clock still says the next frame is ahead of
	// it: one that has been pushed to a frame rate it can't keep up with spends every frame behind
	// its clock, so the limiter is never ticked and the target is never read again. The
	// per-game present paths call this once per rendered frame so normal, fast-forward and
	// slow-down targets keep following the game's inputs even in that state.
	static void PollTargetFPS();

	// How long one of the limiter's frames is, and what the display is running at
	static LARGE_INTEGER FrameWait();
	static unsigned int DisplayRefresh();

	// Where the current frame started, i.e. where the limiter's wait for it ended. The frame
	// interpolation spreads the extra presentations over the frame starting from here, so they
	// can't push the frame past its own length (which would make the limiter fall behind).
	static LARGE_INTEGER FrameStart();

	// How long the game's own part of the last frame took. The extra presentations cost about as
	// much as the game's own pass does, so the interpolation uses this to tell whether there is
	// still room for another presentation before the frame is over.
	static LARGE_INTEGER FrameWork();

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

#ifndef OILP_X64
	// The number of presentations per second the frame interpolation should be putting on the
	// display, worked out from the two interpolation settings. 0 means there is nothing to
	// interpolate:
	//   Interpolation = 1      the display's own refresh rate (a 144hz display with a 60fps game is
	//                          presented 144 times a second, whatever the game's frame rate is)
	//   Interpolation = -1     the largest whole multiple of the game's frame rate the display can
	//                          show, which keeps the presentations evenly spaced (a 180hz display and
	//                          a 60fps game give 3)
	//   Interpolation = N      present N times per game frame
	//   InterpolationFPS = R   present at R frames per second (e.g. *144)
	//
	// The games that had frame interpolation before this worked it out in their own file; the ones
	// that come after can call this instead.
	static double PresentRate();

	// How many extra presentations the frame that just presented should get, and how far apart they
	// should sit (in performance counter units, 0 when there is no room for any). A rate that isn't
	// a multiple of the game's frame rate has its remainder carried over from frame to frame, so
	// the presentations still average out ("*144" with 60fps logic presents 2 and 3 alternately).
	static unsigned int ExtraPresentations(double rate, LARGE_INTEGER& spacing);
	static void ResetPresentationCarry();

	// Whether the patch is presenting every Nth frame instead of every one, because the target
	// frame rate is above what the display can show (see ShouldPresent). The frame interpolation
	// must not add presentations while it is: the thinning is what keeps the presents from
	// throttling the logic, and extra presentations would undo it.
	static bool PresentingEveryNthFrame();
#endif

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
	static LARGE_INTEGER frame_work;
	static unsigned int present_every; // 1 = present every frame
	static unsigned int display_refresh;
	static bool refresh_queried;
};
