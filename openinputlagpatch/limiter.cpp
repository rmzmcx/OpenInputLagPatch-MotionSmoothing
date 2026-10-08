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

// Initializes the limiter's timers, settings, etc
void Limiter::Initialize(ReplayCallback callback) {
	replay_callback = callback;
	QueryPerformanceFrequency(&perf_freq);
	QueryPerformanceCounter(&start_time);

	last_wait_amount.QuadPart = 0;
	frame_start.QuadPart = 0;
	frame_end.QuadPart = 0;

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
		__int64 frame_elapsed = now.QuadPart - frame_start.QuadPart;
		frame_work.QuadPart = frame_elapsed;
		if (++frame_num % 30 == 0) {
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

		if (frame_num % 30 == 0) {
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
		if (Config::D3D9Ex && d3d9_device && !temp_fps_change)
			((IDirect3DDevice9Ex*)d3d9_device)->WaitForVBlank(0);
		QueryPerformanceCounter(&cur_time);
		start_time.QuadPart = cur_time.QuadPart;
		frame_num = 0;
	}

	// Record the frame start time
	QueryPerformanceCounter(&frame_start);
}
