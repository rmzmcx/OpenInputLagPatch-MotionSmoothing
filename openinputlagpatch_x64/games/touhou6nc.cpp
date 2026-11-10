#include <Windows.h>
#include <stdint.h>
#include <stdio.h>
#include "config.h"
#include "d3d11_hook.h"
#include "limiter.h"
#include "patch_util.h"
#include "touhou6nc.h"
#include "x64_config.h"

namespace {
constexpr uintptr_t kRenderChainRva = 0x3BF70;
constexpr uintptr_t kUpdateChainRva = 0x3BE80;
constexpr uintptr_t kScreenFlipRva = 0x953E0;
constexpr uintptr_t kPresentWrapperRva = 0x26FFF0;
constexpr uintptr_t kPresentCallRva = 0x25A1F1;
constexpr uintptr_t kFrameRenderCallRva = 0x45E91;
constexpr uintptr_t kGameUpdateCallRva = 0x45D82;
constexpr uintptr_t kFrameFinishCallRva = 0x3C457;
constexpr uintptr_t kGameFrameLimiterRva = 0x3C45C;
constexpr uintptr_t kAfterGameFrameLimiterRva = 0x3C5A1;
constexpr uintptr_t kFactory2SwapChainCallRva = 0x25333F;
constexpr uintptr_t kFactory1SwapChainCallRva = 0x25356D;
constexpr uintptr_t kFactory2SwapChainWrapperRva = 0x270730;
constexpr uintptr_t kFactory1SwapChainWrapperRva = 0x270A60;
constexpr uintptr_t kReplayIndicatorRva = 0x6BC30;
constexpr uintptr_t kReplayRateTextPatchRva = 0x6BE3C;
constexpr uintptr_t kGameModeRva = 0xC21D9C;
constexpr uint32_t kMenuMode = 1;
constexpr uint32_t kGameplayMode = 2;
constexpr uintptr_t kInputStateRva = 0xA6EC60;
constexpr uintptr_t kReplayControllerRva = 0xA6EC38;
constexpr size_t kReplayActiveOffset = 8;
constexpr uintptr_t kReplayModeRva = 0x4F278C;
constexpr uintptr_t kReplaySpeedBlockedRva = 0x4F27B0;
constexpr uintptr_t kBulletUpdateBlockedRva = 0x4F27B9;
constexpr uintptr_t kInputUpdateRva = 0x12BE0;
constexpr uintptr_t kInputUpdateCallRva = 0x79D32;
constexpr uintptr_t kReplayFastLoopCompareRva = 0x45DDF;
constexpr uintptr_t kReplayFastLoopBranchRva = 0x45DBC;
constexpr uint32_t kInputFocusMask = 0x4;
constexpr uint32_t kInputShootMask = 0x8000;
constexpr uintptr_t kAudioStreamRva = 0x50966C;
constexpr uintptr_t kAudioLoopFlagRva = 0x50967C;
constexpr uintptr_t kAudioLoopPointsFlagRva = 0x50967D;
constexpr uintptr_t kAudioLoopPointsRva = 0x509680;
constexpr uintptr_t kAudioPositionRva = 0xC4A10;
constexpr uintptr_t kAudioLengthRva = 0xC4F90;
constexpr uintptr_t kAudioSeekRva = 0xC9930;
constexpr int64_t kAudioBytesPerGameFrame = 800;
constexpr uintptr_t kPlayerStateRva = 0x4FF3A0;
constexpr uintptr_t kPlayerX = 30512;
constexpr uintptr_t kPlayerY = 30516;
constexpr unsigned int kDiagFrameCapacity = 300;

uintptr_t game_base;
bool installed;
bool flip_discard_hook_installed;
bool replay_indicator_hook_installed;
bool update_chain_hook_installed;
bool input_update_hook_installed;
bool replay_fast_rate_hook_installed;
bool practice_tool_loaded;
char* replay_rate_text;
bool logic_update_ran = true;
uint32_t live_input_state;

double replay_slow_credit;
double replay_slow_debt;
bool replay_slow_active;
int replay_slow_stream = -1;
int64_t replay_slow_bgm_start = -1;
int64_t replay_slow_frames;
int64_t replay_last_bgm_position = -1;
int64_t replay_last_bgm_target = -1;

struct DiagFrame {
	LARGE_INTEGER present;
	uint32_t game_mode;
	bool update_ran;
	bool replay_slow;
};

DiagFrame diag_frames[kDiagFrameCapacity];
uint64_t diag_frame_count;
bool diag_key_down;
LARGE_INTEGER diag_frequency;
LARGE_INTEGER diag_start;

uint8_t* game_address(uintptr_t rva) {
	return (uint8_t*)(game_base + rva);
}

bool call_targets(uint8_t* site, uintptr_t target) {
	if (site[0] != 0xE8)
		return false;
	int32_t displacement = 0;
	memcpy(&displacement, site + 1, sizeof(displacement));
	return (uintptr_t)(site + 5) + displacement == target;
}

using CreateSwapChainForHwndFunction = HRESULT(__fastcall*)(
	IUnknown*, HWND, const DXGI_SWAP_CHAIN_DESC1*,
	const DXGI_SWAP_CHAIN_FULLSCREEN_DESC*,
	IDXGIOutput*, IDXGISwapChain1**);
using CreateSwapChainFunction = HRESULT(__fastcall*)(
	DXGI_SWAP_CHAIN_DESC*, IDXGISwapChain**);
CreateSwapChainForHwndFunction create_swapchain_for_hwnd_original;
CreateSwapChainFunction create_swapchain_original;

HRESULT __fastcall create_swapchain_for_hwnd_flip(
	IUnknown* device, HWND window, const DXGI_SWAP_CHAIN_DESC1* desc,
	const DXGI_SWAP_CHAIN_FULLSCREEN_DESC* fullscreen, IDXGIOutput* output,
	IDXGISwapChain1** swapchain) {
	if (create_swapchain_for_hwnd_original == nullptr || desc == nullptr)
		return E_POINTER;
	DXGI_SWAP_CHAIN_DESC1 adjusted = *desc;
	d3d11_prepare_flip_discard(&adjusted);
	HRESULT result = create_swapchain_for_hwnd_original(
		device, window, &adjusted, fullscreen, output, swapchain);
	if (FAILED(result)) {
		printf("D3D11: FLIP_DISCARD creation failed (0x%08lx), retrying the game desc\n",
			(unsigned long)result);
		result = create_swapchain_for_hwnd_original(
			device, window, desc, fullscreen, output, swapchain);
	}
	return result;
}

HRESULT __fastcall create_swapchain_flip(
	DXGI_SWAP_CHAIN_DESC* desc, IDXGISwapChain** swapchain) {
	if (create_swapchain_original == nullptr || desc == nullptr)
		return E_POINTER;
	DXGI_SWAP_CHAIN_DESC adjusted = *desc;
	d3d11_prepare_flip_discard(&adjusted);
	HRESULT result = create_swapchain_original(&adjusted, swapchain);
	if (FAILED(result)) {
		printf("D3D11: legacy FLIP_DISCARD creation failed (0x%08lx), retrying the game desc\n",
			(unsigned long)result);
		result = create_swapchain_original(desc, swapchain);
	}
	return result;
}

double ticks_to_ms(uint64_t ticks) {
	return diag_frequency.QuadPart != 0
		? (double)ticks * 1000.0 / (double)diag_frequency.QuadPart
		: 0.0;
}

const char* game_mode_name(uint32_t mode) {
	switch (mode) {
	case kMenuMode:
		return "menu";
	case kGameplayMode:
		return "gameplay";
	default:
		return "other";
	}
}

bool replay_is_active(const void* controller) {
	return controller != nullptr &&
		*(const uint8_t*)((const uint8_t*)controller +
			kReplayActiveOffset) != 0;
}

char* allocate_near_game(size_t size) {
	const uintptr_t offsets[] = {
		0x10000000, 0x20000000, 0x30000000, 0x40000000,
	};
	for (uintptr_t offset : offsets) {
		const uintptr_t hint = (game_base + offset) & ~(uintptr_t)0xFFFF;
		void* memory = VirtualAlloc((void*)hint, size,
			MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
		if (memory == nullptr)
			continue;
		const intptr_t delta = (intptr_t)memory -
			(intptr_t)(game_address(kReplayRateTextPatchRva) + 7);
		if (delta >= INT32_MIN && delta <= INT32_MAX)
			return (char*)memory;
		VirtualFree(memory, 0, MEM_RELEASE);
	}
	return nullptr;
}

bool install_replay_rate_text_patch() {
	uint8_t* site = game_address(kReplayRateTextPatchRva);
	static const uint8_t expected[] = { 0x48, 0x8D, 0x1D };
	if (memcmp(site, expected, sizeof(expected)) != 0)
		return false;
	replay_rate_text = allocate_near_game(0x1000);
	if (replay_rate_text == nullptr)
		return false;
	sprintf_s(replay_rate_text, 16, "x%u", X64Config::ReplayFastRate);
	const int32_t displacement = (int32_t)(
		(intptr_t)replay_rate_text -
		(intptr_t)(site + 7));
	patch_bytes(site + 3, (void*)&displacement, sizeof(displacement));
	return true;
}

using ReplayIndicatorFunction = int64_t(__fastcall*)(void*, void*);
ReplayIndicatorFunction replay_indicator_original;

using InputUpdateFunction = uint32_t(__fastcall*)();
InputUpdateFunction input_update_original;

uint32_t __fastcall input_update_hook() {
	live_input_state = input_update_original();
	return live_input_state;
}

bool install_replay_fast_rate_patch() {
	uint8_t* compare = game_address(kReplayFastLoopCompareRva);
	uint8_t* branch = game_address(kReplayFastLoopBranchRva);
	static const uint8_t expected_compare[] = {
		0x83, 0xFF, 0x07, 0x7C, 0xB3,
	};
	static const uint8_t expected_branch[] = { 0x74, 0x3C };
	if (memcmp(compare, expected_compare, sizeof(expected_compare)) != 0 ||
		memcmp(branch, expected_branch, sizeof(expected_branch)) != 0) {
		return false;
	}
	if (X64Config::ReplayFastRate == 1) {
		const uint8_t skip_loop = 0xEB;
		patch_bytes(branch, (void*)&skip_loop, sizeof(skip_loop));
	} else {
		const uint8_t extra_updates =
			(uint8_t)(X64Config::ReplayFastRate - 1);
		patch_bytes(compare + 2, (void*)&extra_updates,
			sizeof(extra_updates));
	}
	return true;
}

int64_t __fastcall replay_indicator_hook(void* controller, void* render) {
	if (replay_indicator_original == nullptr)
		return 0;
	if (replay_rate_text != nullptr) {
		if (replay_slow_active)
			sprintf_s(replay_rate_text, 16, "x%.3g",
				X64Config::ReplaySlowRate);
		else
			sprintf_s(replay_rate_text, 16, "x%u",
				X64Config::ReplayFastRate);
	}
	uint32_t* input = (uint32_t*)game_address(kInputStateRva);
	const uint32_t saved_input = *input;
	if (replay_slow_active)
		*input |= 0x8000;
	const int64_t result = replay_indicator_original(controller, render);
	*input = saved_input;
	return result;
}

void align_bgm_after_replay_slow() {
	const int stream = *(int*)game_address(kAudioStreamRva);
	if (stream < 0 || replay_slow_frames <= 0)
		return;

	using AudioPositionFunction = int64_t(__fastcall*)(int);
	using AudioLengthFunction = int64_t(__fastcall*)(int);
	using AudioSeekFunction = int64_t(__fastcall*)(int64_t, int);
	const int64_t position =
		((AudioPositionFunction)game_address(kAudioPositionRva))(stream);
	if (position < 0)
		return;

	int64_t target = replay_slow_bgm_start >= 0
		? replay_slow_bgm_start +
			replay_slow_frames * kAudioBytesPerGameFrame
		: position - (int64_t)(
			replay_slow_debt * kAudioBytesPerGameFrame + 0.5);
	const bool looping =
		*(uint8_t*)game_address(kAudioLoopFlagRva) != 0;
	const bool use_loop_points =
		*(uint8_t*)game_address(kAudioLoopPointsFlagRva) != 0;
	const int64_t loop_start =
		*(int64_t*)game_address(kAudioLoopPointsRva);
	const int64_t loop_end =
		*(int64_t*)(game_address(kAudioLoopPointsRva) +
			sizeof(int64_t));
	if (target < 0)
		target = 0;
	if (looping && use_loop_points && loop_end > loop_start) {
		const int64_t loop_length = loop_end - loop_start;
		while (target >= loop_end)
			target -= loop_length;
	} else {
		const int64_t length =
			((AudioLengthFunction)game_address(kAudioLengthRva))(stream);
		if (length > 0 && target >= length)
			return;
	}
	replay_last_bgm_position = position;
	replay_last_bgm_target = target;
	((AudioSeekFunction)game_address(kAudioSeekRva))(target, stream);
}

bool replay_slow_requested() {
	void* replay_controller =
		*(void**)game_address(kReplayControllerRva);
	const uint32_t input =
		*(uint32_t*)game_address(kInputStateRva);
	return Config::ReplaySpeedControl &&
		input_update_hook_installed &&
		replay_is_active(replay_controller) &&
		*(uint8_t*)game_address(kReplayModeRva) != 0 &&
		*(uint8_t*)game_address(kReplaySpeedBlockedRva) == 0 &&
		*(uint8_t*)game_address(kBulletUpdateBlockedRva) == 0 &&
		*(uint32_t*)game_address(kGameModeRva) == kGameplayMode &&
		(input & kInputShootMask) == 0 &&
		(live_input_state & kInputFocusMask) != 0;
}

void begin_replay_slow() {
	using AudioPositionFunction = int64_t(__fastcall*)(int);
	const int stream = *(int*)game_address(kAudioStreamRva);
	replay_slow_stream = stream;
	replay_slow_bgm_start = stream >= 0
		? ((AudioPositionFunction)game_address(kAudioPositionRva))(stream)
		: -1;
	replay_slow_frames = 0;
	replay_slow_credit = 0.0;
	replay_slow_debt = 0.0;
	replay_slow_active = true;
	replay_last_bgm_position = -1;
	replay_last_bgm_target = -1;
}

void end_replay_slow() {
	align_bgm_after_replay_slow();
	replay_slow_active = false;
	replay_slow_stream = -1;
	replay_slow_bgm_start = -1;
	replay_slow_frames = 0;
	replay_slow_credit = 0.0;
	replay_slow_debt = 0.0;
}

int64_t __fastcall update_chain_hook(void* chain) {
	using UpdateChainFunction = int64_t(__fastcall*)(void*);
	auto update_chain =
		(UpdateChainFunction)game_address(kUpdateChainRva);

	if (!replay_slow_requested()) {
		if (replay_slow_active)
			end_replay_slow();
		replay_slow_credit = 0.0;
		logic_update_ran = true;
		return update_chain(chain);
	}

	bool run_update = false;
	if (!replay_slow_active) {
		begin_replay_slow();
		run_update = true;
	} else {
		const int stream = *(int*)game_address(kAudioStreamRva);
		if (stream != replay_slow_stream) {
			begin_replay_slow();
			run_update = true;
		} else {
			replay_slow_credit += (double)X64Config::ReplaySlowRate;
			if (replay_slow_credit >= 1.0 - 1e-9) {
				replay_slow_credit -= 1.0;
				run_update = true;
			}
		}
	}

	logic_update_ran = run_update;
	if (!run_update)
		return 0;
	++replay_slow_frames;
	replay_slow_debt = (double)replay_slow_frames *
		(1.0 / (double)X64Config::ReplaySlowRate - 1.0);
	return update_chain(chain);
}

void write_diagnostic() {
	wchar_t path[1024] = {};
	if (!GetModuleFileNameW(nullptr, path, _countof(path)))
		return;
	wchar_t* slash = wcsrchr(path, L'\\');
	if (slash == nullptr)
		return;
	slash[1] = L'\0';
	wcscat_s(path, L"oilp_diag.txt");

	FILE* file = nullptr;
	if (_wfopen_s(&file, path, L"w") != 0 || file == nullptr)
		return;

	const unsigned int count = diag_frame_count < kDiagFrameCapacity
		? (unsigned int)diag_frame_count
		: kDiagFrameCapacity;
	const unsigned int start = diag_frame_count < kDiagFrameCapacity
		? 0
		: (unsigned int)(diag_frame_count % kDiagFrameCapacity);
	const uint8_t* player = game_address(kPlayerStateRva);
	const uint32_t game_mode =
		*(const uint32_t*)game_address(kGameModeRva);
	const uint32_t input =
		*(const uint32_t*)game_address(kInputStateRva);
	const void* replay_controller =
		*(void* const*)game_address(kReplayControllerRva);

	fprintf(file, "oilp x64 runtime diagnostics\n");
	fprintf(file, "version: th06nc_old, module base 0x%p, process %lu\n",
		(void*)game_base, (unsigned long)GetCurrentProcessId());
	fprintf(file,
		"logic frame rate %.2f, display refresh %uHz\n",
		Limiter::game_fps, Limiter::DisplayRefresh());
	fprintf(file,
		"config: GameFPS %u, ReplaySpeedControl %d, "
		"ReplaySlowRate %.3f, ReplayFastRate %u, "
		"ShowOverlay %d, DebugConsole %d, "
		"ForceFlipDiscard %d\n",
		Config::GameFPS, Config::ReplaySpeedControl ? 1 : 0,
		X64Config::ReplaySlowRate, X64Config::ReplayFastRate,
		Config::ShowOverlay,
		Config::DebugConsole ? 1 : 0,
		X64Config::ForceFlipDiscard ? 1 : 0);
	fprintf(file,
		"swap chain: FLIP_DISCARD hook %d, current FLIP_DISCARD %d\n",
		flip_discard_hook_installed ? 1 : 0,
		d3d11_swapchain_is_flip_discard() ? 1 : 0);
	fprintf(file,
		"hooks: replay indicator %d, update chain %d, "
		"input update %d, replay fast rate %d\n",
		replay_indicator_hook_installed ? 1 : 0,
		update_chain_hook_installed ? 1 : 0,
		input_update_hook_installed ? 1 : 0,
		replay_fast_rate_hook_installed ? 1 : 0);
	fprintf(file, "practice tool module: %d\n",
		practice_tool_loaded ? 1 : 0);
	fprintf(file,
		"game state: mode %u (%s), input 0x%08lx, "
		"live input 0x%08lX, focus %d, shoot %d, "
		"replay controller 0x%p, replay active %d, slow active %d, "
		"slow updates %lld, BGM debt %.2f frame(s)\n",
		game_mode, game_mode_name(game_mode), (unsigned long)input,
		(unsigned long)live_input_state,
		(live_input_state & kInputFocusMask) != 0 ? 1 : 0,
		(live_input_state & kInputShootMask) != 0 ? 1 : 0,
		replay_controller, replay_is_active(replay_controller) ? 1 : 0,
		replay_slow_active ? 1 : 0,
		(long long)replay_slow_frames, replay_slow_debt);
	fprintf(file, "last slow BGM seek: %lld -> %lld\n",
		(long long)replay_last_bgm_position,
		(long long)replay_last_bgm_target);
	fprintf(file, "last update ran: %d\n",
		logic_update_ran ? 1 : 0);
	fprintf(file, "player: position %.3f,%.3f\n",
		*(const float*)(player + kPlayerX),
		*(const float*)(player + kPlayerY));
	fprintf(file, "frame work %.3fms, frame wait %.3fms\n",
		ticks_to_ms((uint64_t)Limiter::FrameWork().QuadPart),
		ticks_to_ms((uint64_t)Limiter::FrameWait().QuadPart));
	fprintf(file, "frames recorded: %llu, showing %u\n",
		(unsigned long long)diag_frame_count, count);

	for (unsigned int i = 0; i < count; ++i) {
		const DiagFrame& frame =
			diag_frames[(start + i) % kDiagFrameCapacity];
		fprintf(file,
			"frame %4u: mode %u, update %d, slow %d, "
			"present %.3fms\n",
			i, frame.game_mode, frame.update_ran ? 1 : 0,
			frame.replay_slow ? 1 : 0,
			ticks_to_ms((uint64_t)frame.present.QuadPart));
	}
	fclose(file);
	printf("Wrote oilp_diag.txt\n");
}

void check_diagnostic_hotkey() {
	const bool down = (GetAsyncKeyState('U') & 0x8000) != 0;
	if (down && !diag_key_down)
		write_diagnostic();
	diag_key_down = down;
}

void render_chain() {
	((void (*)())game_address(kRenderChainRva))();
}

uintptr_t screen_flip() {
	return ((uintptr_t (*)())game_address(kScreenFlipRva))();
}

uintptr_t frame_begin() {
	Limiter::Tick();
	render_chain();
	return 0;
}

uintptr_t frame_finish() {
	const uintptr_t result = screen_flip();
	Limiter::MarkFrameWorkEnd();
	DiagFrame& frame =
		diag_frames[(size_t)(diag_frame_count % kDiagFrameCapacity)];
	LARGE_INTEGER now;
	QueryPerformanceCounter(&now);
	if (diag_start.QuadPart == 0)
		diag_start = now;
	frame.present.QuadPart = now.QuadPart - diag_start.QuadPart;
	frame.game_mode = *(uint32_t*)game_address(kGameModeRva);
	frame.update_ran = logic_update_ran;
	frame.replay_slow = replay_slow_active;
	++diag_frame_count;
	check_diagnostic_hotkey();
	return result;
}
}

void th06nc_install_patches() {
	if (installed)
		return;
	installed = true;
	game_base = (uintptr_t)GetModuleHandleW(nullptr);
	if (game_base == 0) {
		printf("TH06NC: game module is unavailable\n");
		return;
	}
	practice_tool_loaded =
		GetModuleHandleW(L"th06nc_test.dll") != nullptr;
	QueryPerformanceFrequency(&diag_frequency);

	uint8_t* input_update_call = game_address(kInputUpdateCallRva);
	if (call_targets(input_update_call, game_base + kInputUpdateRva)) {
		input_update_original =
			(InputUpdateFunction)game_address(kInputUpdateRva);
		patch_call(input_update_call, (void*)&input_update_hook);
		input_update_hook_installed = true;
	} else {
		printf("TH06NC: input update call at RVA %p does not match\n",
			(void*)kInputUpdateCallRva);
	}

	replay_fast_rate_hook_installed = install_replay_fast_rate_patch();
	if (!replay_fast_rate_hook_installed) {
		printf("TH06NC: replay fast-forward rate patch at RVA %p does not match\n",
			(void*)kReplayFastLoopCompareRva);
	}

	if (install_replay_rate_text_patch()) {
		static const uint8_t expected_indicator_prologue[] = {
			0x40, 0x53, 0x48, 0x81, 0xEC, 0x60, 0x01, 0x00, 0x00,
		};
		uint8_t* indicator = game_address(kReplayIndicatorRva);
		if (memcmp(indicator, expected_indicator_prologue,
				sizeof(expected_indicator_prologue)) == 0) {
			replay_indicator_original =
				(ReplayIndicatorFunction)install_x64_detour(
					indicator, (void*)&replay_indicator_hook,
					sizeof(expected_indicator_prologue));
			replay_indicator_hook_installed =
				replay_indicator_original != nullptr;
		}
	}

	if (X64Config::ForceFlipDiscard) {
		uint8_t* factory2_call =
			game_address(kFactory2SwapChainCallRva);
		uint8_t* factory1_call =
			game_address(kFactory1SwapChainCallRva);
		create_swapchain_for_hwnd_original =
			(CreateSwapChainForHwndFunction)game_address(
				kFactory2SwapChainWrapperRva);
		create_swapchain_original =
			(CreateSwapChainFunction)game_address(
				kFactory1SwapChainWrapperRva);
		if (call_targets(factory2_call,
				game_base + kFactory2SwapChainWrapperRva)) {
			patch_call(factory2_call,
				(void*)&create_swapchain_for_hwnd_flip);
			flip_discard_hook_installed = true;
		} else {
			printf("TH06NC: DXGI factory2 swap-chain call at RVA %p does not match\n",
				(void*)kFactory2SwapChainCallRva);
		}
		if (call_targets(factory1_call,
				game_base + kFactory1SwapChainWrapperRva)) {
			patch_call(factory1_call, (void*)&create_swapchain_flip);
		} else {
			printf("TH06NC: legacy DXGI swap-chain call at RVA %p does not match\n",
				(void*)kFactory1SwapChainCallRva);
		}
	}

	uint8_t* d3d11_present_call = game_address(kPresentCallRva);
	if (!call_targets(d3d11_present_call,
			game_base + kPresentWrapperRva) ||
		!d3d11_install_present_hook((uintptr_t)d3d11_present_call)) {
		printf("TH06NC: D3D11 Present call at RVA %p does not match\n",
			(void*)kPresentCallRva);
		return;
	}

	uint8_t* render_call = game_address(kFrameRenderCallRva);
	if (call_targets(render_call, game_base + kRenderChainRva)) {
		patch_call(render_call, (void*)&frame_begin);
	} else {
		printf("TH06NC: render-chain call at RVA %p does not match\n",
			(void*)kFrameRenderCallRva);
	}

	uint8_t* update_call = game_address(kGameUpdateCallRva);
	if (call_targets(update_call, game_base + kUpdateChainRva)) {
		patch_call(update_call, (void*)&update_chain_hook);
		update_chain_hook_installed = true;
	} else {
		printf("TH06NC: update-chain call at RVA %p does not match\n",
			(void*)kGameUpdateCallRva);
	}

	uint8_t* finish_call = game_address(kFrameFinishCallRva);
	if (call_targets(finish_call, game_base + kScreenFlipRva)) {
		patch_call(finish_call, (void*)&frame_finish);
	} else {
		printf("TH06NC: ScreenFlip call at RVA %p does not match\n",
			(void*)kFrameFinishCallRva);
	}

	uint8_t* limiter = game_address(kGameFrameLimiterRva);
	if (limiter[0] == 0xFF && limiter[1] == 0x15) {
		patch_jmp(limiter, game_address(kAfterGameFrameLimiterRva));
		const uint8_t nop = 0x90;
		patch_bytes(limiter + 5, (void*)&nop, sizeof(nop));
	} else {
		printf("TH06NC: built-in frame limiter at RVA %p does not match\n",
			(void*)kGameFrameLimiterRva);
	}

	printf("TH06NC: x64 runtime entry installed\n");
}

FPSTarget th06nc_replay_callback() {
	return FPSTarget::Game;
}
