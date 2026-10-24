// Basic frame limiter
// Heavily simplified version of vpatch's limiter with no fancy autobltpreparetime

#include <Windows.h>
#include <stdio.h>
#include "d3d9_hook.h"
#include "config.h"
#include "common.h"
#include "d3d9_overlay.h"
#include "limiter.h"
#include "overlay.h"

bool Limiter::initialized = false;
LARGE_INTEGER Limiter::start_time;
unsigned int Limiter::frame_num = 0;
LARGE_INTEGER Limiter::wait_amount;
LARGE_INTEGER Limiter::last_wait_amount;
LARGE_INTEGER Limiter::blt_prepare_time;
LARGE_INTEGER Limiter::perf_freq;
ReplayCallback Limiter::replay_callback = nullptr;
LARGE_INTEGER Limiter::frame_start;
LARGE_INTEGER Limiter::frame_end;
LARGE_INTEGER Limiter::frame_work;
unsigned int Limiter::present_every = 1;
unsigned int Limiter::display_refresh = 0;
bool Limiter::refresh_queried = false;
double Limiter::game_fps = 60.0;
UINT (*Limiter::external_game_fps)() = nullptr;

bool Limiter::ShouldPresent() {
	if (present_every <= 1)
		return true;
	return (frame_num % present_every) == 0;
}

bool Limiter::PresentingEveryNthFrame() {
	return present_every > 1;
}

// The rate the frame interpolation should be presenting at (see limiter.h). This is what the games
// that had interpolation before this worked out for themselves; the ones that come after call it.
double Limiter::PresentRate() {
	if (Config::InterpolationFPS)
		return (double)Config::InterpolationFPS;

	// "1" asks for the display's own refresh rate: as many presentations a second as the display can
	// show, whatever the game's frame rate is (60fps logic on a 144hz display is presented 144 times
	// a second, which is 2 and 3 presentations a frame, carried over from frame to frame by
	// ExtraPresentations below). The display's rate is the one the limiter read for itself; before it
	// has, there is nothing to work from and the multiple below decides instead.
	if (Config::Interpolation == 1 && display_refresh > 0)
		return (double)display_refresh;

	// "-1" picks the largest multiple of the game's frame rate the display can show, and a value of 2
	// or more is that multiple (0, the off setting, leaves nothing to present)
	int multiplier = Config::Interpolation;
	if (multiplier < 0 || multiplier == 1) {
		UINT display = display_refresh;
		UINT logic = (UINT)(game_fps + 0.5);
		if (display == 0 || logic == 0)
			return 0.0;
		multiplier = (int)(display / logic);
	}
	if (multiplier < 2)
		return 0.0;

	return game_fps * (double)multiplier;
}

// How many extra presentations the frame gets, and how far apart they sit (see limiter.h)
unsigned int Limiter::ExtraPresentations(double rate, LARGE_INTEGER& spacing) {
	spacing.QuadPart = 0;
	if (rate <= 0.0 || game_fps <= 0.0)
		return 0;

	// The game's own presentation is one of the frame's presentations, and the fraction is carried
	// over to the next frame so a rate that isn't a multiple of the game's frame rate still
	// averages out. One game runs per process, so the carry can live here rather than in each game.
	static double presentations_owed = 0.0;
	presentations_owed += rate / game_fps - 1.0;

	unsigned int extras = 0;
	while (presentations_owed >= 1.0 - 1e-9 && extras < 32) {
		presentations_owed -= 1.0;
		++extras;
	}
	if (extras == 0)
		return 0;

	// In the steady state the game's own presents are one frame apart, so spreading the extras
	// this far apart from the one that just happened puts them evenly between this frame and the
	// next. Evenly spaced presentations are what keeps the motion looking even, since each one
	// draws the state a little further along than the one before it.
	spacing.QuadPart = wait_amount.QuadPart / (LONGLONG)(extras + 1);
	return extras;
}

// Initializes the limiter's timers, settings, etc
void Limiter::Initialize(ReplayCallback callback) {
	replay_callback = callback;
	QueryPerformanceFrequency(&perf_freq);
	QueryPerformanceCounter(&start_time);

	last_wait_amount.QuadPart = 0;
	frame_start.QuadPart = 0;
	frame_end.QuadPart = 0;

	// Which of the overlay's two readouts are shown is a setting of its own (see Config::ShowOverlay),
	// and the rate one only means anything while the frames are actually being presented more than
	// once per game frame: with the interpolation off the readout would just be the game's own frame
	// rate, so it stays off whatever the setting says. (The presentation counting itself lives in the
	// D3D8 and D3D9 hooks, which see every present.) Set here because this is the one place every game
	// goes through, and it happens before any overlay is created.
	overlay_show_present_rate((Config::ShowOverlay & 1) != 0 &&
	                          (Config::Interpolation != 0 || Config::InterpolationFPS != 0));

	initialized = true;
}

// Updates the limiter's parameters to reflect things such as replay skipping or external FPS changes via the API
// Returns true if the player is skipping or slowing down a replay
bool Limiter::UpdateTargetFPS() {
	// The frame rate the game runs at when nothing special is going on. On the games that run
	// their own clock off a frame rate, an external tool (thprac) may be the one setting it, and
	// then its value takes priority over the configured one - the game is still paced by this
	// limiter, so both have to agree on what a frame is
	UINT base_target = Config::GameFPS;
	if (external_game_fps) {
		UINT external = external_game_fps();
		if (external > 0)
			base_target = external;
	}

	// Replay skipping/slowing always goes by the patch's own values, which is also what the
	// oilp_set_replay_*_fps API writes to
	UINT target = base_target;
	if (Config::ReplaySpeedControl && replay_callback) {
		switch (replay_callback()) {
			case FPSTarget::Game:
				break;
			case FPSTarget::ReplaySkip:
				target = Config::ReplaySkipFPS;
				break;
			case FPSTarget::ReplaySlow:
				target = Config::ReplaySlowFPS;
				break;
		}
	}

	// Games that keep their own clock read the frame rate from here (see touhou19.cpp). They're
	// paced by the same target as the limiter, replay skipping/slowing included
	game_fps = (double)target;

	wait_amount.QuadPart = (LONGLONG)((double)perf_freq.QuadPart / (double)target);
	blt_prepare_time.QuadPart = min(wait_amount.QuadPart / 2, perf_freq.QuadPart / 1000 * (LONGLONG)Config::BltPrepareTime);

	// When the target framerate goes above the display refresh rate (replay skip, thprac
	// speedup), presenting every frame would throttle the logic down to the refresh rate -
	// which is exactly what happens under dgVoodoo2, where the present blocks on the
	// D3D11 flip queue and nothing else can pace the game. Present every Nth frame instead.
	if (!refresh_queried) {
		refresh_queried = true;
		DEVMODEA mode = {};
		mode.dmSize = sizeof(mode);
		if (EnumDisplaySettingsA(NULL, ENUM_CURRENT_SETTINGS, &mode))
			display_refresh = mode.dmDisplayFrequency;
	}
	present_every = 1;
	if (display_refresh > 0 && target > display_refresh)
		present_every = (target + display_refresh - 1) / display_refresh;

	return target != base_target;
}

// Exposed function for outside tools such as thprac to set the framerate
bool Limiter::SetGameFPS(int fps) {
	if (!initialized)
		return false;
	if (fps <= 0)
		return false;
	Config::GameFPS = fps;
	game_fps = (double)fps;
	return true;
}

// Simple spinwait function
// As precise as possible, but eats up lots of CPU
inline void spin_wait(__int64 target) {
	LARGE_INTEGER cur_time;
	QueryPerformanceCounter(&cur_time);
	while (cur_time.QuadPart < target)
		QueryPerformanceCounter(&cur_time);
}

// Half-spinwait, half-timer wait from vpatch
// Creates a waitable timer until 1 ms before the target, then spins for the rest
// Timer accuracy check not included
bool half_spin_wait_inited = false;
__int64 timer_1ms = 0; // 1 ms relative to the performance counter frequency
double timer_freq_scale = 0; // Used for converting from performance counter -> FILETIME
HANDLE waitable_timer = NULL;
inline void half_spin_wait(__int64 target) {
	if (!half_spin_wait_inited) {
		LARGE_INTEGER freq;
		QueryPerformanceFrequency(&freq);
		timer_1ms = freq.QuadPart / 1000;
		timer_freq_scale = 10000000.0 / (double)freq.QuadPart;
		waitable_timer = CreateWaitableTimer(NULL, TRUE, NULL);
		if (waitable_timer == NULL)
			panic_msgbox(L"Failed to create waitable timer. GLE: 0x%x", GetLastError());
		half_spin_wait_inited = true;
	}

	LARGE_INTEGER cur_time;
	QueryPerformanceCounter(&cur_time);
	auto diff = target - cur_time.QuadPart;
	if (diff >= 0) {
		if (diff >= timer_1ms) {
			// Negative to indicate relative time for SetWaitableTimer
			auto wait_amount = (__int64)((double)(diff - timer_1ms) * -timer_freq_scale);
			if (wait_amount < 0) {
				SetWaitableTimer(waitable_timer, (LARGE_INTEGER*)&wait_amount, 0, NULL, NULL, FALSE);
				WaitForSingleObject(waitable_timer, INFINITE);
			}
		}
		spin_wait(target);
	}
}

// Re-reads the frame rate an external tool is asking for (see limiter.h)
void Limiter::PollExternalGameFPS() {
	if (initialized)
		UpdateTargetFPS();
}

// TODO: Implement https://blat-blatnik.github.io/computerBear/making-accurate-sleep-function/

// Waits until the given point in time (see limiter.h)
void Limiter::WaitUntil(LARGE_INTEGER target) {
	switch (Config::Sleep) {
		case SleepType::Spin:
			spin_wait(target.QuadPart);
			break;
		case SleepType::Vpatch:
			half_spin_wait(target.QuadPart);
			break;
	}
}

// How long one of the limiter's frames is (see limiter.h)
LARGE_INTEGER Limiter::FrameWait() {
	return wait_amount;
}

// The frame rate measurement the games that measure their own are told about (see limiter.h and
// Config::CalcFPS). The games' own counters are taken over, so this is what counts their frames in
// their place: it adds one per game frame and works the rate out in the same half second windows
// their own code uses, with the same timing source (timeGetTime), so the number the game is handed
// looks like the one its own reading would have produced.
//
// One thing it deliberately does not: the games count their draw passes against wall clock time, so a
// frame the game spends stalled inside itself - loading a stage, a scene change - makes the window read
// as a slow one, and the game then carries that as frame rate the run lost. That is the few percent of
// "processing rate" a run with a load in it shows on its results screen and records in a replay, which
// is what vpatch's CalcFPS exists to take out: its stub hands the game the frame rate the run is being
// paced at rather than one it measured for itself. A frame is therefore charged for one frame's worth
// of time here, not for however long a stall made it, which leaves a run that never fell behind reading
// the frame rate it was paced at.
static unsigned int measured_frame_count;
static unsigned int measured_window_start;
static unsigned int measured_last_time;
static unsigned int measured_window_time;
static float measured_fps;

void Limiter::CountGameFrame() {
	++measured_frame_count;

	unsigned int now = timeGetTime();
	if (measured_window_start == 0) {
		measured_window_start = now;
		measured_last_time = now;
		return;
	}

	// How long one frame of the target frame rate is, in the same milliseconds the window is counted in
	// (wait_amount is still zero until the limiter's first Tick has worked it out)
	unsigned int frame_ms = (unsigned int)((double)wait_amount.QuadPart * 1000.0 / (double)perf_freq.QuadPart + 0.5);
	if (frame_ms == 0)
		frame_ms = 1;

	unsigned int delta = now - measured_last_time;
	measured_last_time = now;
	
	// A frame that took longer than any frame can is a stall inside the game rather than frame rate the
	// run lost (see the comment above), so it is charged as one frame
	if (delta > frame_ms * 2)
		delta = frame_ms;
	measured_window_time += delta;
	
	if (measured_window_time >= 500) {
		measured_fps = (float)measured_frame_count * 1000.0f / (float)measured_window_time;
		measured_frame_count = 0;
		measured_window_time = 0;
	}
}

float Limiter::MeasuredFps() {
	// Until the first window has closed the game is told the frame rate it is being paced to, which
	// is what it would have read off a run that never fell behind
	return measured_fps != 0.0f ? measured_fps : (float)game_fps;
}

// Where the game's own part of the current frame ended, if it was reported (see limiter.h)
static LARGE_INTEGER frame_work_end;

void Limiter::MarkFrameWorkEnd() {
	QueryPerformanceCounter(&frame_work_end);
}

// What the display is running at (see limiter.h)
unsigned int Limiter::DisplayRefresh() {
	return display_refresh;
}

// Where the current frame started (see limiter.h)
LARGE_INTEGER Limiter::FrameStart() {
	return frame_start;
}

// How long the game's own part of the last frame took (see limiter.h)
LARGE_INTEGER Limiter::FrameWork() {
	return frame_work;
}

// Same as Tick, but waits for a specific amount of time instead of the limiter's own schedule.
// th19 and th20 keep a clock of their own and only run a frame once that clock says one is due.
// Instead of building a schedule of its own - which can and does end up out of phase with the
// game's clock, pushing the frames, and with them the input-to-present latency, around - the
// limiter is handed what the game's clock says is left of the frame and waits exactly that.
void Limiter::TickUntil(double seconds) {
	if (!initialized)
		panic_msgbox(L"Tried to tick the limiter before initialization.");

	LARGE_INTEGER now;
	QueryPerformanceCounter(&now);

	// Calculate how much time it took for the game to process this frame
	if (frame_start.QuadPart != 0) {
		// The games with frame interpolation report where their own part of the frame ended, so
		// the extra presentations the patch adds - which are the patch's work, not the game's -
		// don't show up in the frame time the overlay reports. Without this, the readout is the
		// whole frame including the extras, and so reads about as many times too high as the game
		// is presented per frame. (Same as Tick() below.)
		__int64 frame_end_time = frame_work_end.QuadPart != 0 ? frame_work_end.QuadPart : now.QuadPart;
		frame_work_end.QuadPart = 0;
		__int64 frame_elapsed = frame_end_time - frame_start.QuadPart;
		frame_work.QuadPart = frame_elapsed;
		// The render time line is one of the overlay's two readouts and can be turned off on its own
		// (see Config::ShowOverlay); frame_num is counted either way, the limiter paces by it
		const bool show_time_text = (++frame_num % 30) == 0 && (Config::ShowOverlay & 2) != 0;
		if (show_time_text) {
			char overlay_text[64];
			sprintf_s(overlay_text, "%.2f/%.2fms", frame_elapsed / (float)perf_freq.QuadPart * 1000.0, (float)Config::BltPrepareTime);
			unsigned long overlay_color = frame_elapsed > blt_prepare_time.QuadPart ? 0xFFFF0000 : 0xFFFFFFFF;

			if (D3D9Overlay::Instance) {
				D3D9Overlay::Instance->SetText("%s", overlay_text);
				D3D9Overlay::Instance->text_color = overlay_color;
			} else if (d3d8_overlay_active()) {
				d3d8_overlay_set_text(overlay_text, overlay_color);
			}
		}
	}

	// UpdateTargetFPS keeps the frame rate the game's clock runs at in sync with the config
	UpdateTargetFPS();

	// Wait out whatever the game's clock says is left of the frame
	if (seconds > 0.0) {
		LARGE_INTEGER target;
		target.QuadPart = now.QuadPart + (LONGLONG)(seconds * (double)perf_freq.QuadPart);

		switch (Config::Sleep) {
			case SleepType::Spin:
				spin_wait(target.QuadPart);
				break;
			case SleepType::Vpatch:
				half_spin_wait(target.QuadPart);
				break;
		}
	}

	QueryPerformanceCounter(&frame_start);
}

// Performs the actual frame limiting
void Limiter::Tick() {
	if (!initialized)
		panic_msgbox(L"Tried to tick the limiter before initialization.");

	// One frame of the game is about to run, which is what the measurement the games' own frame rate
	// counters are replaced with counts (see CountGameFrame). This is the one place that is reached
	// once per frame and never for an extra presentation, so the extra presentations the frame
	// interpolation adds cannot end up in the frame rate the game is told about.
	CountGameFrame();

	LARGE_INTEGER now;
	QueryPerformanceCounter(&now);

	// Calculate how much time it took for the game to process this frame
	__int64 frame_elapsed = 0;
	if (frame_start.QuadPart != 0) {
		// Games with frame interpolation report where their own part of the frame ended, so the
		// extra presentations the patch adds don't show up in the overlay's frame time
		__int64 frame_end_time = frame_work_end.QuadPart != 0 ? frame_work_end.QuadPart : now.QuadPart;
		frame_work_end.QuadPart = 0;
		frame_elapsed = frame_end_time - frame_start.QuadPart;
		frame_work.QuadPart = frame_elapsed;

		// See the note on the same block in TickUntil above: the render time line can be off on its own
		if ((Config::ShowOverlay & 2) != 0 && frame_num % 30 == 0) {
			char overlay_text[64];
			sprintf_s(overlay_text, "%.2f/%.2fms", frame_elapsed / (float)perf_freq.QuadPart * 1000.0, (float)Config::BltPrepareTime);
			unsigned long overlay_color = frame_elapsed > blt_prepare_time.QuadPart ? 0xFFFF0000 : 0xFFFFFFFF;

			if (D3D9Overlay::Instance) {
				D3D9Overlay::Instance->SetText("%s", overlay_text);
				D3D9Overlay::Instance->text_color = overlay_color;
			} else if (d3d8_overlay_active()) {
				d3d8_overlay_set_text(overlay_text, overlay_color);
			}
		}
	}

	// Set up the target time before returning
	bool temp_fps_change = UpdateTargetFPS();
	__int64 target = start_time.QuadPart + ++frame_num * wait_amount.QuadPart;
	if (!temp_fps_change) // Don't care about input latency if skipping/slowing down a replay
		target -= blt_prepare_time.QuadPart;

	// Perform the frame limiting
	LARGE_INTEGER cur_time;
	QueryPerformanceCounter(&cur_time);

	// Only resync the timer if a full frame has been skipped
	if (target + wait_amount.QuadPart >= cur_time.QuadPart && last_wait_amount.QuadPart == wait_amount.QuadPart) {
		switch (Config::Sleep) {
			case SleepType::Spin:
				spin_wait(target);
				break;
			case SleepType::Vpatch:
				half_spin_wait(target);
				break;
		}
	} else {
		printf("Frame limiter fell behind or target FPS has changed. Resyncing...\n");
		last_wait_amount.QuadPart = wait_amount.QuadPart;
		IDirect3DDevice9* device9 = d3d9_hooked_device();
		if (Config::D3D9Ex && device9 && !temp_fps_change)
			((IDirect3DDevice9Ex*)device9)->WaitForVBlank(0);
		QueryPerformanceCounter(&cur_time);
		start_time.QuadPart = cur_time.QuadPart;
		frame_num = 0;
	}

	// Record the frame start time
	QueryPerformanceCounter(&frame_start);
}
