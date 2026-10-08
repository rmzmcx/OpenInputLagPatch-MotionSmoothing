// Touhou 15: Legacy of Lunatic Kingdom v1.00b

#include <stdio.h>
#include "patch_util.h"
#include "limiter.h"
#include "d3d9_hook.h"
#include "config.h"
#include "tool_state.h"
#include "touhou15.h"

using namespace Touhou15;

CReplayManager** CReplayManager::InstancePtr = (CReplayManager**)0x004E9BC4;

int __fastcall th15_window_update_hook(void* self) {
	Limiter::Tick();
	return CWindowManager__UpdateFast(self);
}

// ---------------------------------------------------------------------------
// Frame interpolation, stage 0: extra presentations
// ---------------------------------------------------------------------------
//
// th15 draws a game frame once per frame, with a block that sits in the frame function of the
// scene that's running:
//
//     0x4724A6  CALL [device+0xA4]        BeginScene
//     0x4724B4  FUN_0047E3A0              reset the sprite queue
//     0x4724B9  FUN_0044D630(0x4E77D0)    prepare the render context
//     0x4724CD  FUN_00401620              render every object (they fill the sprite queue)
//     0x4724D8  FUN_0047E3F0([0x503C18])  draw the sprite queue
//     0x4724E9  CALL [device+0x104]       SetTexture(0, NULL)
//     0x4724F7  CALL [device+0xA8]        EndScene
//
// and presents later in the same frame:
//
//     0x4726EA  ...                       Present (through [device+0x44])
//
// Both blocks appear three times, once for the game scene and once for each of the two menu
// scenes: the renders are at 0x4724CD, 0x4728DD and 0x472A7C, the presents at 0x4726EA, 0x472946
// and 0x472B5C, and each present is laid out identically (the device is loaded into eax, the
// arguments are pushed, then the present is called).
//
// zun's engine separates the game's logic from its rendering, so the same state can be drawn again
// by running the render block again - that's what the extra presentations are. The game's own
// present is the first one, and the extra ones are spread evenly over the rest of the frame, right
// where the game would otherwise sit and wait for the next one, so the presents land on the
// display's refresh evenly:
//
//     |-----------------------|-----------------------|-----------------------|
//     present                 present                 present                 present
//     (the game's own)        (extra)                 (extra)                 (the next frame's)
//
// The projection of the drawn objects - moving them forward in time before the extra presentations
// - comes with stage 1. With stage 0 the extra presentations draw the state the game is already in,
// which is the "moved forward by zero frames" case, and is what the infrastructure - the pacing,
// the re-rendering, the overlays of the user's tools, the diagnostics - is validated with.

// The plain calls the render block makes into the game
static auto th15_reset_sprite_queue = (void(*)())0x0047E3A0;
static auto th15_prepare_render = (void(__fastcall*)(void*))0x0044D630; // ECX = the render context
static auto th15_render_objects = (void(*)())0x00401620;                // renders every object
static auto th15_draw_sprite_queue = (void(__fastcall*)(void*))0x0047E3F0; // ECX = the queue's owner

// Where the user's tool builds its frame and where it draws it. thprac's th15 module starts its
// frame (its NewFrame, its UI code, and its EndFrame) from a hook on the return of the scene
// update at 0x4015FA, and draws the overlay it built from a hook on the return of the object
// render at 0x40170A. The extra presentations render the scene again, which erases the overlay the
// game's own pass drew, so the tool has to draw it again for every one of them (see tool_state.h -
// its frame state is found at runtime and set again there, which draws the overlay without running
// any of the tool's own code).
static unsigned char* const th15_tool_frame_address = (unsigned char*)0x004015FA;
static unsigned char* const th15_tool_draw_address = (unsigned char*)0x0040170A;

// The viewport the scene is drawn with, taken from the frame the game's own render ran in: the
// extra presentations have to draw with the same one, and it can differ from what's left over in
// the device (the pause screens, for example, render into a smaller part of the back buffer)
static D3DVIEWPORT9 th15_scene_viewport = {};

// Remembers the viewport the scene is about to be drawn with, then lets the game's own render run
// exactly as it always did (see th15_extra_presentation for what it's for)
static void th15_render_hook() {
	IDirect3DDevice9* device = d3d9_hooked_device();
	if (device != nullptr)
		device->GetViewport(&th15_scene_viewport);

	// The tool's frame is built by now (it runs from the scene update's return, before this) but
	// not drawn yet (that happens on the return of the render below): the point to watch it from
	ToolState::WatchFrameReady(th15_tool_frame_address, th15_tool_draw_address);

	th15_render_objects();
}

// The number of presentations per second the config asks for. 0 means interpolation is off.
//
// "-1" picks the largest multiple of the game's frame rate that still fits the display refresh
// (a 190hz display and a 60fps game give 3), a positive value is that multiple, and the
// "*<frame rate>" form is that frame rate no matter what the game's own is.
static double th15_present_rate() {
	if (Config::InterpolationFPS)
		return (double)Config::InterpolationFPS;

	int multiplier = Config::Interpolation;
	if (multiplier < 0) {
		UINT display = Limiter::DisplayRefresh();
		UINT logic = (UINT)(Limiter::game_fps + 0.5);
		if (display == 0 || logic == 0)
			return 0.0;
		multiplier = (int)(display / logic);
	}
	if (multiplier < 2)
		return 0.0;
	return Limiter::game_fps * (double)multiplier;
}

// Diagnostics: press U to write the presentation timing of the last frames to oilp_diag.txt
static const unsigned int th15_diag_frames_max = 300;
static const unsigned int th15_diag_presents_max = 8;
struct Th15DiagFrame {
	LARGE_INTEGER base;                                      // the game's own present of the frame
	unsigned int presents;
	LARGE_INTEGER present_time[th15_diag_presents_max];      // when each presentation happened
};
static Th15DiagFrame th15_diag_frames[th15_diag_frames_max];
static unsigned int th15_diag_count;
static bool th15_diag_key_down;

static LARGE_INTEGER th15_perf_freq;

static void th15_diag_dump() {
	wchar_t path[1024] = {};
	if (!GetModuleFileNameW(NULL, path, MAX_PATH))
		return;
	wchar_t* slash = wcsrchr(path, L'\\');
	if (slash)
		slash[1] = L'\0';
	wcscat_s(path, L"oilp_diag.txt");

	FILE* file = nullptr;
	if (_wfopen_s(&file, path, L"w") != 0 || file == nullptr)
		return;

	unsigned int count = th15_diag_count < th15_diag_frames_max ? th15_diag_count : th15_diag_frames_max;
	unsigned int start = th15_diag_count < th15_diag_frames_max ? 0 : th15_diag_count % th15_diag_frames_max;

	fprintf(file, "oilp frame interpolation diagnostics\n");
	fprintf(file, "logic frame rate: %.2f, configured presentation rate: %.2f\n",
		Limiter::game_fps, th15_present_rate());
	fprintf(file, "frames recorded: %u\n", count);

	double presents = 0.0;
	double first = 0.0;
	double last = 0.0;
	for (unsigned int i = 0; i < count; ++i) {
		const Th15DiagFrame& frame = th15_diag_frames[(start + i) % th15_diag_frames_max];
		fprintf(file, "frame %4u: %u present(s)", i, frame.presents);
		for (unsigned int p = 0; p < frame.presents && p < th15_diag_presents_max; ++p)
			fprintf(file, " [%u] %.2fms", p,
				(double)(frame.present_time[p].QuadPart - frame.base.QuadPart) * 1000.0 / (double)th15_perf_freq.QuadPart);
		fprintf(file, "\n");
		presents += frame.presents;
		if (i == 0)
			first = (double)frame.base.QuadPart;
		last = (double)frame.base.QuadPart;
	}
	if (last > first)
		fprintf(file, "average: %.3f presents per frame, %.1f presentations per second\n",
			presents / (double)count, presents * (double)th15_perf_freq.QuadPart / (last - first));

	fclose(file);
	printf("Wrote oilp_diag.txt\n");
}

// One extra presentation of the frame: the same render block the game runs, once more, so the
// state the game is in is drawn and presented again (stage 0; stage 1 will project the objects
// forward in time first). The tool's frame is readied again, so the object render below draws its
// overlay into this presentation the same way it does in the game's own one.
static void th15_extra_presentation() {
	IDirect3DDevice9* device = d3d9_hooked_device();
	if (device == nullptr)
		return;

	device->BeginScene();
	// The game's own pass set the viewport before this point, and the pause screens and the menus
	// change it, so put back whatever the scene was drawn with (see th15_render_hook)
	if (th15_scene_viewport.Width != 0)
		device->SetViewport(&th15_scene_viewport);
	th15_reset_sprite_queue();
	*(DWORD*)0x004E81E4 = 0xFF;
	th15_prepare_render((void*)0x004E77D0);
	ToolState::ReadyToDraw();
	th15_render_objects();
	th15_draw_sprite_queue(*(void**)0x00503C18);
	device->SetTexture(0, nullptr);
	device->EndScene();

	// The extras are presented with the device's own Present, without the hooks the tools put in
	// front of it (see d3d9_present_bypassing_hooks)
	d3d9_present_bypassing_hooks();
}

// Presents the frame the game just drew, and then the extra presentations the config asked for.
// This is what the game's own present call runs instead of: the call is replaced with a call to
// this, and the present the game sets up after it (pushing the arguments and calling the device)
// is skipped over, so the game ends up right after its own present call again, with the result in
// eax where its code expects it.
static HRESULT th15_present_hook() {
	IDirect3DDevice9* device = d3d9_hooked_device();
	if (device == nullptr)
		return E_FAIL;

	static bool freq_queried = false;
	if (!freq_queried) {
		freq_queried = true;
		QueryPerformanceFrequency(&th15_perf_freq);
	}

	LARGE_INTEGER base;
	QueryPerformanceCounter(&base);

	// The game's own presentation of this frame, through the device it drew with, so that the
	// hooks the user's tools have on it still see it
	HRESULT result = device->Present(nullptr, nullptr, nullptr, nullptr);

	// The game's own work of this frame ends here as far as the overlay is concerned - everything
	// below is the patch presenting the same frame again
	Limiter::MarkFrameWorkEnd();

	// The tool has drawn its frame by now: the other half of watching for its frame state
	ToolState::WatchFrameDrawn();

	unsigned int presents = 1;
	LARGE_INTEGER present_time[th15_diag_presents_max];
	present_time[0] = base;

	double rate = th15_present_rate();
	// Nothing more to present if the presentation that just happened failed (a lost device, for
	// example) - the game's own code right after this handles that
	if (SUCCEEDED(result) && rate > 0.0 && Limiter::game_fps > 0.0) {
		static bool rate_printed = false;
		if (!rate_printed) {
			rate_printed = true;
			printf("Frame interpolation: %.1f presentations per second (%.2f per game frame)\n",
				rate, rate / Limiter::game_fps);
		}

		// How many presentations this frame gets. The game's own present is one of them, and the
		// fraction is carried over to the next frame, so a rate that isn't a multiple of the game's
		// frame rate still averages out ("*144" with 60fps logic presents 2 and 3 alternately).
		static double presentations_owed = 0.0;
		presentations_owed += rate / Limiter::game_fps - 1.0;
		unsigned int extras = 0;
		while (presentations_owed >= 1.0 - 1e-9 && extras < 32) {
			presentations_owed -= 1.0;
			++extras;
		}

		LARGE_INTEGER frame_wait = Limiter::FrameWait();
		if (extras > 0 && frame_wait.QuadPart > 0) {
			// The game's own present is the first presentation of the frame; the extras go after
			// it, one every this many performance counter ticks, evenly spread over the rest of
			// the frame. Keeping the presentations evenly spaced matters: each one draws the state
			// a little further along, so an uneven spacing is an uneven looking motion.
			double spacing = (double)frame_wait.QuadPart / (double)(extras + 1);
			if (spacing >= 1.0) {
				// The presentations can't run past the end of the frame either - the limiter's
				// schedule is absolute, so a frame that overruns is a frame the game falls behind
				// on. An extra presentation costs about as much as the game's own pass did, so if
				// the last ones wouldn't fit before the next frame starts they're dropped (the
				// frame is then presented fewer times, but never late).
				LARGE_INTEGER frame_start = Limiter::FrameStart();
				LARGE_INTEGER frame_work = Limiter::FrameWork();
				bool has_deadline = frame_start.QuadPart > 0 && frame_work.QuadPart > 0;
				LARGE_INTEGER deadline;
				deadline.QuadPart = frame_start.QuadPart + frame_wait.QuadPart;

				for (unsigned int k = 1; k <= extras; ++k) {
					LARGE_INTEGER target;
					target.QuadPart = base.QuadPart + (LONGLONG)(spacing * (double)k + 0.5);
					if (has_deadline && target.QuadPart + frame_work.QuadPart > deadline.QuadPart)
						continue;

					Limiter::WaitUntil(target);

					if (presents < th15_diag_presents_max)
						QueryPerformanceCounter(&present_time[presents]);
					++presents;

					th15_extra_presentation(); // stage 0: the same state again
				}
			}
		}
	}

	// Record the frame for the diagnostics
	{
		Th15DiagFrame& frame = th15_diag_frames[th15_diag_count % th15_diag_frames_max];
		frame.base = base;
		frame.presents = presents;
		for (unsigned int p = 0; p < th15_diag_presents_max; ++p)
			frame.present_time[p] = p < presents ? present_time[p] : base;
		++th15_diag_count;
	}

	// U dumps the diagnostics
	if (GetAsyncKeyState('U') & 0x8000) {
		if (!th15_diag_key_down) {
			th15_diag_key_down = true;
			th15_diag_dump();
		}
	} else {
		th15_diag_key_down = false;
	}

	return result;
}

// The present the game sets up at each present call site: the arguments are pushed and the device
// is called, 14 bytes in all (see th15_install_patches)
static const size_t th15_present_setup_size = 14;

// Jumps over the present the game sets up after its present call site, so the code after it runs
// with the result the hook left in eax
static void th15_skip_present_setup(DWORD address) {
	BYTE patch[th15_present_setup_size];
	patch[0] = 0xE9;
	*(DWORD*)(patch + 1) = (address + th15_present_setup_size) - (address + 5);
	for (size_t i = 5; i < th15_present_setup_size; ++i)
		patch[i] = 0x90;
	patch_bytes(address, patch, sizeof(patch));
}

void th15_install_patches() {
	{
		// Skip the original frame limiter
		BYTE patch[] = { 0xEB, 0x4A };
		patch_bytes(0x004727DE, patch, sizeof(patch));
	}
	{
		// Force fast input latency mode
		BYTE patch[] = { 0xEB };
		patch_bytes(0x00471A86, patch, sizeof(patch)); // Skip over automatic
		patch_bytes(0x00471A9B, patch, sizeof(patch)); // Skip over normal
	}
	{
		// Hook window update
		patch_call(0x00471AB7, th15_window_update_hook);
	}
	if (Config::D3D9Ex) {
		// Redirect Direct3DCreate9 call
		// IAT is being hooked, but thcrap also hooks Direct3DCreate9 in the same place
		// This should force our Direct3DCreate9 hook to be loaded no matter what
		patch_call(0x0047158C, Direct3DCreate9_hook);

		// Extra NOP is needed because we're replacing a FF 15 call, which is 6 bytes long
		BYTE patch[] = { 0x90 };
		patch_bytes(0x00471591, patch, sizeof(patch));
	}
	if (Config::ReplaySpeedControl) {
		// Skip the original replay speed control stuff
		BYTE patch[] = { 0xEB, 0x1D };
		patch_bytes(0x0045CED2, patch, sizeof(patch));
	}
	if (Config::Interpolation || Config::InterpolationFPS) {
		// Take over the object render, only to remember the viewport the scene is drawn with (see
		// th15_render_hook); the render itself runs exactly the way it always did
		static const DWORD render_calls[] = { 0x004724CD, 0x004728DD, 0x00472A7C };
		unsigned int render_patched = 0;
		for (DWORD address : render_calls) {
			BYTE* render_call = (BYTE*)address;
			if (render_call[0] == 0xE8) {
				patch_call(address, (void*)th15_render_hook);
				++render_patched;
			} else {
				printf("th15 render call at 0x%x doesn't look like a known one, skipping!\n", address);
			}
		}

		// Take over the presents in the three frame functions. The instruction that loads the
		// device for the present is 5 bytes, which is exactly what the hook's call needs, and the
		// 14 bytes after it (the arguments and the present call itself) are skipped over - the hook
		// presents with the same device and arguments the game would have used.
		static const DWORD present_sites[] = { 0x004726EA, 0x00472946, 0x00472B5C };
		unsigned int present_patched = 0;
		for (DWORD address : present_sites) {
			BYTE* present_site = (BYTE*)address;
			if (present_site[0] == 0xA1 && *(DWORD*)(present_site + 1) == 0x004E77D8) {
				patch_call(address, (void*)th15_present_hook);
				th15_skip_present_setup(address + 5);
				++present_patched;
			} else {
				printf("th15 present call at 0x%x doesn't look like a known one, skipping!\n", address);
			}
		}
		printf("Frame interpolation: %u render call(s) and %u present call(s) taken over (setting %d)\n",
			render_patched, present_patched,
			Config::InterpolationFPS ? (int)Config::InterpolationFPS : Config::Interpolation);
	}
}

FPSTarget th15_replay_callback() {
	if (*CReplayManager::InstancePtr && (*CReplayManager::InstancePtr)->mode == 1) {
		// TODO: Reverse engineer this struct instead of being lazy
		auto input = *(DWORD*)0x004E6D10;
		if (input & InputState::Focus)
			return FPSTarget::ReplaySlow;
		else if (input & (InputState::Skip | InputState::Shoot))
			return FPSTarget::ReplaySkip;
	}
	return FPSTarget::Game;
}
