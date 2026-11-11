#include <Windows.h>
#include <stdint.h>
#include <stdio.h>
#include <math.h>
#include <string.h>
#include "common.h"
#include "config.h"
#include "d3d11_hook.h"
#include "limiter.h"
#include "patch_util.h"
#include "player_interpolation.h"
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
constexpr uintptr_t kFrameComposeCallRva = 0x45E96;
constexpr uintptr_t kFrameComposeRva = 0x3C330;
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
constexpr uintptr_t kReplayFastLoopImmediateRva =
	kReplayFastLoopCompareRva + 2;
constexpr uint32_t kInputFocusMask = 0x4;
constexpr uint32_t kInputShootMask = 0x8000;
constexpr uintptr_t kThpracShiftHeldRva = 0xC21F9A;
constexpr uintptr_t kThpracFarTriggerHeldRva = 0x54601C;
constexpr uintptr_t kFpsLimiterConstantRva = 0xC22100;
constexpr uintptr_t kAudioStreamRva = 0x50966C;
constexpr uintptr_t kAudioLoopFlagRva = 0x50967C;
constexpr uintptr_t kAudioLoopPointsFlagRva = 0x50967D;
constexpr uintptr_t kAudioLoopPointsRva = 0x509680;
constexpr uintptr_t kAudioPositionRva = 0xC4A10;
constexpr uintptr_t kAudioLengthRva = 0xC4F90;
constexpr uintptr_t kAudioSeekRva = 0xC9930;
constexpr int64_t kAudioBytesPerGameFrame = 800;
constexpr uintptr_t kNativeFpsTextRva = 0xC221D0;
constexpr uintptr_t kNativeFpsCounterRva = 0xC2232C;
constexpr uintptr_t kNativeFpsWindowStartRva = 0xC29648;
constexpr uintptr_t kNativeFpsLostFastRva = 0xC21D0C;
constexpr uintptr_t kNativeFpsLostSlowRva = 0xC21D10;
constexpr uintptr_t kPlayerStateRva = 0x4FF3A0;
constexpr uintptr_t kPlayerX = 30512;
constexpr uintptr_t kPlayerY = 30516;
constexpr uintptr_t kPlayerLeftLevelRva = 0x4FF0E0;
constexpr uintptr_t kPlayerTopLevelRva = 0x4FF0E4;
constexpr uintptr_t kPlayerWidthLevelRva = 0x4FF0E8;
constexpr uintptr_t kPlayerHeightLevelRva = 0x4FF0EC;
constexpr uintptr_t kPlayerStateOffset = 0x7898;
constexpr uintptr_t kPlayerMoveMultiplierX = 0x7710;
constexpr uintptr_t kPlayerMoveMultiplierY = 0x7714;
constexpr uintptr_t kPlayerSpeed = 0x7860;
constexpr uintptr_t kPlayerFocusSpeed = 0x7864;
constexpr uintptr_t kPlayerDiagonal = 0x7868;
constexpr uintptr_t kPlayerFocusDiagonal = 0x786C;
constexpr uintptr_t kPlayerOptionsOffset = 0x7880;
constexpr size_t kPlayerOptionSize = 12;
constexpr uintptr_t kPlayerShotsOffset = 0x410;
constexpr unsigned int kPlayerShotCount = 80;
constexpr size_t kPlayerShotSize = 0x170;
constexpr uintptr_t kPlayerShotStateOffset = 0x10;
constexpr uintptr_t kPlayerShotTypeOffset = 0x12;
constexpr uintptr_t kPlayerShotDrawPositionOffset = 0xE0;
constexpr uintptr_t kPlayerShotLogicalPositionOffset = 0x13C;
constexpr float kPlayerMaxStep = 48.0f;
constexpr float kPlayerShotMaxStep = 16.0f;
constexpr uintptr_t kLaserSlotsRva = 0x4E12B8;
constexpr unsigned int kLaserSlotCount = 64;
constexpr size_t kLaserSlotSize = 0x298;
constexpr uintptr_t kLaserWidthOffset = 0x04;
constexpr uintptr_t kLaserStartOffset = 0x10;
constexpr uintptr_t kLaserEndOffset = 0x14;
constexpr uintptr_t kLaserAngleOffset = 0x260;
constexpr uintptr_t kLaserSpriteOffset = 0x264;
constexpr uintptr_t kLaserActiveOffset = 0x274;
constexpr uintptr_t kLaserPositionOffset = 0x278;
constexpr uintptr_t kLaserStateOffset = 0x290;
constexpr unsigned int kLaserFieldCount = 6;
constexpr float kLaserMaxStep = 48.0f;
constexpr float kLaserMaxAngleStep = 0.7853981634f;
constexpr uintptr_t kBulletSlotsRva = 0x3EC2A8;
constexpr unsigned int kBulletSlotCount = 640;
constexpr size_t kBulletSlotSize = 0x620;
constexpr uintptr_t kBulletVelocityOffset = 0x08;
constexpr uintptr_t kBulletPositionOffset = 0x30;
constexpr uintptr_t kBulletStateOffset = 0x44;
constexpr uint16_t kBulletStateFlying = 1;
constexpr unsigned int kDiagFrameCapacity = 300;
constexpr unsigned int kMaxExtraPresentations = 32;
constexpr uintptr_t kLogicStateRegions[][2] = {
	{ 0x3EC2A8, 0xF5000 },
	{ 0x4E12B8, 0xA600 },
	{ 0x4FF3A0, 0xA2B8 },
	{ 0xAA1E90, 0x10B000 },
	{ 0xC07240, 0x1A728 },
	{ 0xC25520, 0x3D18 },
};
constexpr size_t kLogicStateSize =
	0xF5000 + 0xA600 + 0xA2B8 + 0x10B000 + 0x1A728 + 0x3D18;

struct ThpracInterpolationVersion {
	const char* sha256;
	uintptr_t game_gui_progress_rva;
};

constexpr ThpracInterpolationVersion kThpracInterpolationVersions[] = {
	{
		"8355cf6731fbbc86c76044a632e5e8c4eeefdb54a10489c7ab83273aa4e11c3b",
		0x183D20,
	},
};

uintptr_t game_base;
bool installed;
bool flip_discard_hook_installed;
bool replay_indicator_hook_installed;
bool update_chain_hook_installed;
bool input_update_hook_installed;
bool replay_fast_rate_hook_installed;
bool logic_update_hook_installed;
bool replay_rate_text_patch_installed;
bool thprac_compatibility_ready;
bool thprac_interpolation_checked;
bool practice_tool_loaded;
bool thprac_focus_mirrored;
HMODULE thprac_module;
volatile LONG* thprac_game_gui_progress;
char* replay_rate_text;
int32_t replay_rate_text_original_displacement;
bool logic_update_ran;
uint32_t live_input_state;
uint64_t logic_updates_this_frame;
uint32_t current_frame_updates;
uint32_t current_frame_extras_requested;
uint32_t current_frame_extras_presented;
uint32_t current_frame_bullets_projected;
uint32_t current_frame_lasers_projected;
uint32_t current_frame_player_shots_projected;
uint32_t current_frame_state_restores;
bool current_frame_player_projected;
uint64_t interpolation_extra_count;
uint64_t total_logic_updates;
uint64_t extra_logic_updates;
double interpolation_presentation_owed;
bool extra_presentation_active;
bool extra_render_active;
LARGE_INTEGER normal_presentation_base;
bool game_foreground;
bool game_foreground_known;

double replay_slow_credit;
double replay_slow_debt;
bool replay_slow_active;
int replay_slow_stream = -1;
int64_t replay_slow_bgm_start = -1;
int64_t replay_slow_frames;
int64_t replay_slow_ticks;
int64_t replay_last_bgm_position = -1;
int64_t replay_last_bgm_target = -1;

struct DiagFrame {
	LARGE_INTEGER present;
	uint32_t game_mode;
	uint32_t updates;
	uint32_t extras_requested;
	uint32_t extras_presented;
	uint32_t bullets_projected;
	uint32_t lasers_projected;
	uint32_t player_shots_projected;
	uint32_t state_restores;
	bool player_projected;
	bool update_ran;
	bool replay_slow;
};

DiagFrame diag_frames[kDiagFrameCapacity];
uint64_t diag_frame_count;
bool diag_key_down;
LARGE_INTEGER diag_frequency;
LARGE_INTEGER diag_start;
LARGE_INTEGER logic_measure_start;

using FrameComposeFunction = uintptr_t(__fastcall*)();
FrameComposeFunction frame_compose_original;
bool frame_compose_hook_installed;
uint8_t* logic_state_snapshot;
uint64_t logic_state_snapshot_hash;

struct ProjectedBullet {
	uint8_t* slot;
	uint8_t original_position[12];
};

ProjectedBullet projected_bullets[kBulletSlotCount];
unsigned int projected_bullet_count;

struct MeasuredStep {
	float last_x;
	float last_y;
	float step_x;
	float step_y;
	bool known;
};

struct PlayerShotMotion {
	MeasuredStep step;
	float last_logical_x;
	float last_logical_y;
	uint16_t last_type;
	bool active_last;
};

struct PlayerDiagnostic {
	uint32_t input;
	float position_x;
	float position_y;
	float measured_x;
	float measured_y;
	float option_x[2];
	float option_y[2];
	float option_step_x[2];
	float option_step_y[2];
	float speed;
	float focus_speed;
	float diagonal;
	float focus_diagonal;
	float multiplier_x;
	float multiplier_y;
	float min_x;
	float max_x;
	float min_y;
	float max_y;
	uint32_t state;
};

MeasuredStep measured_player_step;
MeasuredStep measured_option_steps[2];
PlayerShotMotion player_shot_motion[kPlayerShotCount];
PlayerDiagnostic player_diagnostic;

float saved_player_x;
float saved_player_y;
float saved_option_positions[2][2];
unsigned int saved_option_count;

struct ProjectedPlayerShot {
	uint8_t* slot;
	float original_x;
	float original_y;
};

ProjectedPlayerShot projected_player_shots[kPlayerShotCount];
unsigned int projected_player_shot_count;

struct LaserMotion {
	float last[kLaserFieldCount];
	float step[kLaserFieldCount];
	bool active_last;
};

struct ProjectedLaser {
	uint8_t* slot;
	float original[kLaserFieldCount];
};

LaserMotion laser_motion[kLaserSlotCount];
ProjectedLaser projected_lasers[kLaserSlotCount];
unsigned int projected_laser_count;
unsigned int laser_angle_jumps;

struct NativeFpsState {
	int32_t counter;
	int64_t window_start;
	float lost_fast;
	float lost_slow;
	char text[256];
};

uint8_t* game_address(uintptr_t rva) {
	return (uint8_t*)(game_base + rva);
}

bool replay_is_active(const void* controller);

bool call_targets(uint8_t* site, uintptr_t target) {
	if (site[0] != 0xE8)
		return false;
	int32_t displacement = 0;
	memcpy(&displacement, site + 1, sizeof(displacement));
	return (uintptr_t)(site + 5) + displacement == target;
}

bool thprac_loaded() {
	if (thprac_module != nullptr)
		return true;
	thprac_module = GetModuleHandleW(L"thprac64.dll");
	if (thprac_module == nullptr)
		thprac_module = GetModuleHandleW(L"thprac.dll");
	return thprac_module != nullptr;
}

bool game_window_is_foreground() {
	const HWND foreground = GetForegroundWindow();
	if (foreground == nullptr)
		return false;
	DWORD process_id = 0;
	GetWindowThreadProcessId(foreground, &process_id);
	return process_id == GetCurrentProcessId();
}

void update_foreground_state() {
	const bool foreground = game_window_is_foreground();
	if (game_foreground_known && foreground != game_foreground) {
		interpolation_presentation_owed = 0.0;
	}
	game_foreground = foreground;
	game_foreground_known = true;
}

bool module_rva_is_writable(HMODULE module, uintptr_t rva, size_t size) {
	if (module == nullptr || size == 0)
		return false;
	const uint8_t* base = (const uint8_t*)module;
	const IMAGE_DOS_HEADER* dos = (const IMAGE_DOS_HEADER*)base;
	if (dos->e_magic != IMAGE_DOS_SIGNATURE)
		return false;
	const IMAGE_NT_HEADERS64* nt =
		(const IMAGE_NT_HEADERS64*)(base + dos->e_lfanew);
	if (nt->Signature != IMAGE_NT_SIGNATURE ||
		nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC) {
		return false;
	}
	if (rva + size < rva ||
		rva + size > nt->OptionalHeader.SizeOfImage) {
		return false;
	}
	const IMAGE_SECTION_HEADER* section = IMAGE_FIRST_SECTION(nt);
	for (unsigned int i = 0; i < nt->FileHeader.NumberOfSections;
		++i, ++section) {
		const uintptr_t start = section->VirtualAddress;
		const size_t section_size = section->Misc.VirtualSize != 0
			? section->Misc.VirtualSize
			: section->SizeOfRawData;
		if (!(section->Characteristics & IMAGE_SCN_MEM_WRITE))
			continue;
		if (rva >= start && rva + size <= start + section_size)
			return true;
	}
	return false;
}

void discover_thprac_interpolation_state() {
	if (thprac_interpolation_checked || !thprac_loaded())
		return;
	thprac_interpolation_checked = true;

	wchar_t path[1024] = {};
	if (!GetModuleFileNameW(thprac_module, path, _countof(path))) {
		printf("TH06NC: thprac path unavailable, UI replay disabled\n");
		return;
	}
	char hash[65] = {};
	if (!sha256_file(path, hash)) {
		printf("TH06NC: thprac hash unavailable, UI replay disabled\n");
		return;
	}

	for (const ThpracInterpolationVersion& version :
		kThpracInterpolationVersions) {
		if (strcmp(hash, version.sha256) != 0)
			continue;
		if (!module_rva_is_writable(thprac_module,
				version.game_gui_progress_rva, sizeof(LONG))) {
			printf("TH06NC: thprac UI state is outside a writable image "
				"section, UI replay disabled\n");
			return;
		}
		thprac_game_gui_progress = (volatile LONG*)(
			(uint8_t*)thprac_module + version.game_gui_progress_rva);
		printf("TH06NC: thprac %s UI replay state at RVA 0x%llX\n",
			hash, (unsigned long long)version.game_gui_progress_rva);
		return;
	}

	printf("TH06NC: unsupported thprac hash %s, UI replay disabled\n",
		hash);
}

void prepare_thprac_for_extra_presentation() {
	if (thprac_game_gui_progress == nullptr ||
		*thprac_game_gui_progress != 0) {
		return;
	}
	InterlockedExchange(thprac_game_gui_progress, 2);
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
	d3d11_prepare_flip_discard(&adjusted, X64Config::AllowTearing != FALSE);
	HRESULT result = create_swapchain_for_hwnd_original(
		device, window, &adjusted, fullscreen, output, swapchain);
	if (FAILED(result)) {
		printf("D3D11: FLIP_DISCARD+tearing creation failed (0x%08lx), "
			"retrying without tearing\n",
			(unsigned long)result);
		d3d11_prepare_flip_discard(&adjusted, false);
		result = create_swapchain_for_hwnd_original(
			device, window, &adjusted, fullscreen, output, swapchain);
		if (FAILED(result)) {
			printf("D3D11: FLIP_DISCARD creation failed (0x%08lx), "
				"retrying the game desc\n", (unsigned long)result);
			result = create_swapchain_for_hwnd_original(
				device, window, desc, fullscreen, output, swapchain);
		}
	}
	return result;
}

HRESULT __fastcall create_swapchain_flip(
	DXGI_SWAP_CHAIN_DESC* desc, IDXGISwapChain** swapchain) {
	if (create_swapchain_original == nullptr || desc == nullptr)
		return E_POINTER;
	DXGI_SWAP_CHAIN_DESC adjusted = *desc;
	d3d11_prepare_flip_discard(&adjusted, X64Config::AllowTearing != FALSE);
	HRESULT result = create_swapchain_original(&adjusted, swapchain);
	if (FAILED(result)) {
		printf("D3D11: legacy FLIP_DISCARD+tearing creation failed "
			"(0x%08lx), retrying without tearing\n",
			(unsigned long)result);
		d3d11_prepare_flip_discard(&adjusted, false);
		result = create_swapchain_original(&adjusted, swapchain);
		if (FAILED(result)) {
			printf("D3D11: legacy FLIP_DISCARD creation failed "
				"(0x%08lx), retrying the game desc\n",
				(unsigned long)result);
			result = create_swapchain_original(desc, swapchain);
		}
	}
	return result;
}

double ticks_to_ms(uint64_t ticks) {
	return diag_frequency.QuadPart != 0
		? (double)ticks * 1000.0 / (double)diag_frequency.QuadPart
		: 0.0;
}

uint64_t hash_bytes(uint64_t hash, const void* data, size_t size) {
	const uint8_t* bytes = (const uint8_t*)data;
	while (size >= sizeof(uint64_t)) {
		uint64_t word = 0;
		memcpy(&word, bytes, sizeof(word));
		hash ^= word;
		hash *= 1099511628211ull;
		hash ^= hash >> 32;
		bytes += sizeof(word);
		size -= sizeof(word);
	}
	while (size-- != 0) {
		hash ^= *bytes++;
		hash *= 1099511628211ull;
	}
	return hash;
}

uint64_t hash_logic_state() {
	uint64_t hash = 1469598103934665603ull;
	for (const auto& region : kLogicStateRegions) {
		hash = hash_bytes(hash, game_address(region[0]), region[1]);
	}
	return hash;
}

bool ensure_logic_state_snapshot() {
	if (logic_state_snapshot != nullptr)
		return true;
	logic_state_snapshot = (uint8_t*)VirtualAlloc(nullptr, kLogicStateSize,
		MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
	return logic_state_snapshot != nullptr;
}

void save_logic_state_snapshot() {
	size_t offset = 0;
	for (const auto& region : kLogicStateRegions) {
		memcpy(logic_state_snapshot + offset, game_address(region[0]),
			region[1]);
		offset += region[1];
	}
	logic_state_snapshot_hash = hash_bytes(1469598103934665603ull,
		logic_state_snapshot, kLogicStateSize);
}

void restore_logic_state_if_changed() {
	if (logic_state_snapshot == nullptr)
		return;
	const uint64_t current_hash = hash_logic_state();
	if (current_hash == logic_state_snapshot_hash)
		return;

	size_t offset = 0;
	for (const auto& region : kLogicStateRegions) {
		memcpy(game_address(region[0]), logic_state_snapshot + offset,
			region[1]);
		offset += region[1];
	}
	++current_frame_state_restores;
}

void advance_flying_bullets(float t) {
	projected_bullet_count = 0;
	for (unsigned int i = 0; i < kBulletSlotCount; ++i) {
		uint8_t* bullet =
			game_address(kBulletSlotsRva) + (size_t)i * kBulletSlotSize;
		if (*(const uint16_t*)(bullet + kBulletStateOffset) !=
			kBulletStateFlying) {
			continue;
		}

		ProjectedBullet& projected = projected_bullets[projected_bullet_count++];
		projected.slot = bullet;
		float* position = (float*)(bullet + kBulletPositionOffset);
		const float* velocity = (const float*)(bullet + kBulletVelocityOffset);
		memcpy(projected.original_position, position,
			sizeof(projected.original_position));
		for (unsigned int axis = 0; axis < 3; ++axis) {
			position[axis] = position[axis] +
				velocity[axis] * t;
		}
	}
	current_frame_bullets_projected = projected_bullet_count;
}

void restore_flying_bullets() {
	for (unsigned int i = 0; i < projected_bullet_count; ++i) {
		const ProjectedBullet& projected = projected_bullets[i];
		memcpy(projected.slot + kBulletPositionOffset,
			projected.original_position,
			sizeof(projected.original_position));
	}
	projected_bullet_count = 0;
}

void read_laser_fields(const uint8_t* slot, float* values) {
	values[0] = *(const float*)(slot + kLaserPositionOffset);
	values[1] = *(const float*)(slot + kLaserPositionOffset + 4);
	values[2] = *(const float*)(slot + kLaserAngleOffset);
	values[3] = *(const float*)(slot + kLaserStartOffset);
	values[4] = *(const float*)(slot + kLaserEndOffset);
	values[5] = *(const float*)(slot + kLaserWidthOffset);
}

float wrap_angle_step(float step) {
	constexpr float two_pi = 6.2831853072f;
	constexpr float pi = 3.1415926536f;
	while (step > pi)
		step -= two_pi;
	while (step < -pi)
		step += two_pi;
	return step;
}

void reset_laser_measurements() {
	memset(laser_motion, 0, sizeof(laser_motion));
	laser_angle_jumps = 0;
}

void measure_lasers() {
	laser_angle_jumps = 0;
	for (unsigned int i = 0; i < kLaserSlotCount; ++i) {
		uint8_t* slot =
			game_address(kLaserSlotsRva) + (size_t)i * kLaserSlotSize;
		const bool active =
			*(const uint8_t*)(slot + kLaserActiveOffset) != 0;
		float values[kLaserFieldCount];
		read_laser_fields(slot, values);
		LaserMotion& motion = laser_motion[i];

		const float dx = values[0] - motion.last[0];
		const float dy = values[1] - motion.last[1];
		const float angle_step =
			wrap_angle_step(values[2] - motion.last[2]);
		const bool position_reused =
			dx * dx + dy * dy >= kLaserMaxStep * kLaserMaxStep;
		const bool angle_jump =
			angle_step >= kLaserMaxAngleStep ||
			angle_step <= -kLaserMaxAngleStep;
		const bool reused =
			motion.active_last && (position_reused || angle_jump);
		if (active && motion.active_last && angle_jump)
			++laser_angle_jumps;

		for (unsigned int field = 0; field < kLaserFieldCount; ++field) {
			motion.step[field] =
				active && motion.active_last && !reused
					? (field == 2
						? angle_step
						: values[field] - motion.last[field])
					: 0.0f;
			motion.last[field] = values[field];
		}
		motion.active_last = active;
	}
}

void advance_lasers(float t) {
	projected_laser_count = 0;
	for (unsigned int i = 0; i < kLaserSlotCount; ++i) {
		uint8_t* slot =
			game_address(kLaserSlotsRva) + (size_t)i * kLaserSlotSize;
		if (*(const uint8_t*)(slot + kLaserActiveOffset) == 0)
			continue;

		ProjectedLaser& projected =
			projected_lasers[projected_laser_count++];
		projected.slot = slot;
		float current[kLaserFieldCount];
		read_laser_fields(slot, current);
		for (unsigned int field = 0; field < kLaserFieldCount; ++field) {
			projected.original[field] = current[field];
			current[field] +=
				laser_motion[i].step[field] * t;
		}
		*(float*)(slot + kLaserPositionOffset) = current[0];
		*(float*)(slot + kLaserPositionOffset + 4) = current[1];
		*(float*)(slot + kLaserAngleOffset) = current[2];
		*(float*)(slot + kLaserStartOffset) = current[3];
		*(float*)(slot + kLaserEndOffset) = current[4];
		*(float*)(slot + kLaserWidthOffset) = current[5];
	}
	current_frame_lasers_projected = projected_laser_count;
}

void restore_lasers() {
	for (unsigned int i = 0; i < projected_laser_count; ++i) {
		const ProjectedLaser& projected = projected_lasers[i];
		uint8_t* slot = projected.slot;
		*(float*)(slot + kLaserPositionOffset) =
			projected.original[0];
		*(float*)(slot + kLaserPositionOffset + 4) =
			projected.original[1];
		*(float*)(slot + kLaserAngleOffset) =
			projected.original[2];
		*(float*)(slot + kLaserStartOffset) =
			projected.original[3];
		*(float*)(slot + kLaserEndOffset) =
			projected.original[4];
		*(float*)(slot + kLaserWidthOffset) =
			projected.original[5];
	}
	projected_laser_count = 0;
}

void measure_step(MeasuredStep* motion, const float* position,
	float max_step) {
	if (motion->known) {
		motion->step_x = position[0] - motion->last_x;
		motion->step_y = position[1] - motion->last_y;
		if (max_step > 0.0f &&
			motion->step_x * motion->step_x +
				motion->step_y * motion->step_y >=
			max_step * max_step) {
			motion->step_x = 0.0f;
			motion->step_y = 0.0f;
		}
	} else {
		motion->step_x = 0.0f;
		motion->step_y = 0.0f;
		motion->known = true;
	}
	motion->last_x = position[0];
	motion->last_y = position[1];
}

void reset_player_measurements() {
	memset(&measured_player_step, 0, sizeof(measured_player_step));
	memset(measured_option_steps, 0, sizeof(measured_option_steps));
	memset(player_shot_motion, 0, sizeof(player_shot_motion));
}

void measure_player_and_shots() {
	uint8_t* player = game_address(kPlayerStateRva);
	player_diagnostic.input =
		*(const uint32_t*)game_address(kInputStateRva);
	player_diagnostic.position_x =
		*(const float*)(player + kPlayerX);
	player_diagnostic.position_y =
		*(const float*)(player + kPlayerY);
	player_diagnostic.speed =
		*(const float*)(player + kPlayerSpeed);
	player_diagnostic.focus_speed =
		*(const float*)(player + kPlayerFocusSpeed);
	player_diagnostic.diagonal =
		*(const float*)(player + kPlayerDiagonal);
	player_diagnostic.focus_diagonal =
		*(const float*)(player + kPlayerFocusDiagonal);
	player_diagnostic.multiplier_x =
		*(const float*)(player + kPlayerMoveMultiplierX);
	player_diagnostic.multiplier_y =
		*(const float*)(player + kPlayerMoveMultiplierY);
	const float left =
		*(const float*)game_address(kPlayerLeftLevelRva);
	const float top =
		*(const float*)game_address(kPlayerTopLevelRva);
	player_diagnostic.min_x = left;
	player_diagnostic.max_x = left +
		*(const float*)game_address(kPlayerWidthLevelRva);
	player_diagnostic.min_y = top;
	player_diagnostic.max_y = top +
		*(const float*)game_address(kPlayerHeightLevelRva);
	player_diagnostic.state =
		*(const uint32_t*)(player + kPlayerStateOffset);
	measure_step(&measured_player_step, (float*)(player + kPlayerX),
		kPlayerMaxStep);
	player_diagnostic.measured_x = measured_player_step.step_x;
	player_diagnostic.measured_y = measured_player_step.step_y;
	for (unsigned int option = 0; option < 2; ++option) {
		float* option_position = (float*)(
			player + kPlayerOptionsOffset +
			option * kPlayerOptionSize);
		measure_step(&measured_option_steps[option], option_position,
			kPlayerMaxStep);
		player_diagnostic.option_x[option] = option_position[0];
		player_diagnostic.option_y[option] = option_position[1];
		player_diagnostic.option_step_x[option] =
			measured_option_steps[option].step_x;
		player_diagnostic.option_step_y[option] =
			measured_option_steps[option].step_y;
	}

	uint8_t* shots = player + kPlayerShotsOffset;
	for (unsigned int i = 0; i < kPlayerShotCount; ++i) {
		uint8_t* shot = shots + (size_t)i * kPlayerShotSize;
		PlayerShotMotion& motion = player_shot_motion[i];
		const uint16_t state = *(const uint16_t*)(
			shot + kPlayerShotStateOffset);
		if (state == 0) {
			motion.active_last = false;
			motion.step.known = false;
			motion.last_type = 0;
			continue;
		}

		float* position =
			(float*)(shot + kPlayerShotDrawPositionOffset);
		const float* logical = (const float*)(
			shot + kPlayerShotLogicalPositionOffset);
		const uint16_t type = *(const uint16_t*)(
			shot + kPlayerShotTypeOffset);
		const float logical_dx =
			logical[0] - motion.last_logical_x;
		const float logical_dy =
			logical[1] - motion.last_logical_y;
		const bool reused =
			motion.active_last && (
				type != motion.last_type ||
				logical_dx * logical_dx + logical_dy * logical_dy >=
					kPlayerShotMaxStep * kPlayerShotMaxStep);
		if (!motion.active_last || reused) {
			motion.step.last_x = position[0];
			motion.step.last_y = position[1];
			motion.step.step_x = 0.0f;
			motion.step.step_y = 0.0f;
			motion.step.known = true;
			motion.active_last = true;
		} else {
			measure_step(&motion.step, position, kPlayerShotMaxStep);
		}
		motion.last_logical_x = logical[0];
		motion.last_logical_y = logical[1];
		motion.last_type = type;
	}
}

bool player_body_prediction_allowed() {
	const void* replay_controller =
		*(void* const*)game_address(kReplayControllerRva);
	return *(const uint32_t*)game_address(kGameModeRva) ==
			kGameplayMode &&
		!replay_is_active(replay_controller) &&
		*(const uint8_t*)game_address(kReplaySpeedBlockedRva) == 0 &&
		*(const uint8_t*)game_address(kBulletUpdateBlockedRva) == 0;
}

bool enemy_object_updates_blocked() {
	return *(const uint8_t*)game_address(kReplaySpeedBlockedRva) != 0 ||
		*(const uint8_t*)game_address(kBulletUpdateBlockedRva) != 0;
}

PlayerInterpolation::Step predicted_player_step() {
	PlayerInterpolation::Step step = { 0.0f, 0.0f };
	if (!player_body_prediction_allowed())
		return step;

	const unsigned int input =
		*(const uint32_t*)game_address(kInputStateRva);
	const PlayerInterpolation::Controls controls =
		PlayerInterpolation::StandardControls(input, kInputFocusMask);
	uint8_t* player = game_address(kPlayerStateRva);
	step = PlayerInterpolation::FromSpeeds(controls.direction,
		controls.focus,
		*(const float*)(player + kPlayerSpeed),
		*(const float*)(player + kPlayerFocusSpeed),
		*(const float*)(player + kPlayerDiagonal),
		*(const float*)(player + kPlayerFocusDiagonal));
	step.x *= *(const float*)(player + kPlayerMoveMultiplierX);
	step.y *= *(const float*)(player + kPlayerMoveMultiplierY);

	const float min_x =
		*(const float*)game_address(kPlayerLeftLevelRva);
	const float min_y =
		*(const float*)game_address(kPlayerTopLevelRva);
	const float max_x = min_x +
		*(const float*)game_address(kPlayerWidthLevelRva);
	const float max_y = min_y +
		*(const float*)game_address(kPlayerHeightLevelRva);
	if (!isfinite(min_x) || !isfinite(max_x) ||
		!isfinite(min_y) || !isfinite(max_y) ||
		min_x >= max_x || min_y >= max_y) {
		step.x = 0.0f;
		step.y = 0.0f;
		return step;
	}
	PlayerInterpolation::ClampStep(
		*(const float*)(player + kPlayerX),
		*(const float*)(player + kPlayerY),
		min_x, max_x, min_y, max_y, &step);
	return step;
}

void advance_player_and_options(float t) {
	uint8_t* player = game_address(kPlayerStateRva);
	float* position = (float*)(player + kPlayerX);
	const PlayerInterpolation::Step predicted = predicted_player_step();
	const float extra_x =
		(predicted.x - measured_player_step.step_x) * t;
	const float extra_y =
		(predicted.y - measured_player_step.step_y) * t;

	saved_player_x = position[0];
	saved_player_y = position[1];
	position[0] += predicted.x * t;
	position[1] += predicted.y * t;

	saved_option_count = 0;
	for (unsigned int option = 0; option < 2; ++option) {
		float* option_position = (float*)(
			player + kPlayerOptionsOffset +
			option * kPlayerOptionSize);
		saved_option_positions[option][0] = option_position[0];
		saved_option_positions[option][1] = option_position[1];
		option_position[0] +=
			measured_option_steps[option].step_x * t + extra_x;
		option_position[1] +=
			measured_option_steps[option].step_y * t + extra_y;
		++saved_option_count;
	}
	current_frame_player_projected = true;
}

void restore_player_and_options() {
	uint8_t* player = game_address(kPlayerStateRva);
	float* position = (float*)(player + kPlayerX);
	position[0] = saved_player_x;
	position[1] = saved_player_y;
	for (unsigned int option = 0; option < saved_option_count; ++option) {
		float* option_position = (float*)(
			player + kPlayerOptionsOffset +
			option * kPlayerOptionSize);
		option_position[0] = saved_option_positions[option][0];
		option_position[1] = saved_option_positions[option][1];
	}
	saved_option_count = 0;
}

void advance_player_shots(float t) {
	projected_player_shot_count = 0;
	uint8_t* player = game_address(kPlayerStateRva);
	uint8_t* shots = player + kPlayerShotsOffset;
	for (unsigned int i = 0; i < kPlayerShotCount; ++i) {
		uint8_t* shot = shots + (size_t)i * kPlayerShotSize;
		if (*(const uint16_t*)(shot + kPlayerShotStateOffset) == 0)
			continue;
		const MeasuredStep& motion = player_shot_motion[i].step;
		if (!motion.known)
			continue;

		ProjectedPlayerShot& projected =
			projected_player_shots[projected_player_shot_count++];
		float* position =
			(float*)(shot + kPlayerShotDrawPositionOffset);
		projected.slot = shot;
		projected.original_x = position[0];
		projected.original_y = position[1];
		position[0] += motion.step_x * t;
		position[1] += motion.step_y * t;
	}
	current_frame_player_shots_projected =
		projected_player_shot_count;
}

void restore_player_shots() {
	for (unsigned int i = 0; i < projected_player_shot_count; ++i) {
		const ProjectedPlayerShot& projected =
			projected_player_shots[i];
		float* position = (float*)(
			projected.slot + kPlayerShotDrawPositionOffset);
		position[0] = projected.original_x;
		position[1] = projected.original_y;
	}
	projected_player_shot_count = 0;
}

void save_native_fps_state(NativeFpsState* state) {
	state->counter =
		*(const int32_t*)game_address(kNativeFpsCounterRva);
	state->window_start =
		*(const int64_t*)game_address(kNativeFpsWindowStartRva);
	state->lost_fast =
		*(const float*)game_address(kNativeFpsLostFastRva);
	state->lost_slow =
		*(const float*)game_address(kNativeFpsLostSlowRva);
	memcpy(state->text, game_address(kNativeFpsTextRva),
		sizeof(state->text));
}

void restore_native_fps_state(const NativeFpsState* state) {
	*(int32_t*)game_address(kNativeFpsCounterRva) = state->counter;
	*(int64_t*)game_address(kNativeFpsWindowStartRva) =
		state->window_start;
	*(float*)game_address(kNativeFpsLostFastRva) = state->lost_fast;
	*(float*)game_address(kNativeFpsLostSlowRva) = state->lost_slow;
	memcpy(game_address(kNativeFpsTextRva), state->text,
		sizeof(state->text));
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
	memcpy(&replay_rate_text_original_displacement, site + 3,
		sizeof(replay_rate_text_original_displacement));
	replay_rate_text = allocate_near_game(0x1000);
	if (replay_rate_text == nullptr)
		return false;
	sprintf_s(replay_rate_text, 16, "x%u", X64Config::ReplayFastRate);
	const int32_t displacement = (int32_t)(
		(intptr_t)replay_rate_text -
		(intptr_t)(site + 7));
	patch_bytes(site + 3, (void*)&displacement, sizeof(displacement));
	replay_rate_text_patch_installed = true;
	return true;
}

void restore_replay_rate_text() {
	if (!replay_rate_text_patch_installed)
		return;
	patch_bytes(game_address(kReplayRateTextPatchRva) + 3,
		(void*)&replay_rate_text_original_displacement,
		sizeof(replay_rate_text_original_displacement));
	replay_rate_text_patch_installed = false;
}

using ReplayIndicatorFunction = int64_t(__fastcall*)(void*, void*);
ReplayIndicatorFunction replay_indicator_original;

using UpdateChainFunction = int64_t(__fastcall*)(void*);
UpdateChainFunction logic_update_original;

using InputUpdateFunction = uint32_t(__fastcall*)();
InputUpdateFunction input_update_original;

void reflect_focus_for_thprac();

uint32_t __fastcall input_update_hook() {
	live_input_state = input_update_original();
	reflect_focus_for_thprac();
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
	if (thprac_loaded())
		return replay_indicator_original(controller, render);
	if (replay_rate_text_patch_installed && replay_rate_text != nullptr) {
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

bool replay_slow_prerequisites() {
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
		(input & kInputShootMask) == 0;
}

bool replay_slow_requested() {
	return replay_slow_prerequisites() &&
		(live_input_state & kInputFocusMask) != 0;
}

bool thprac_replay_slow_requested() {
	if (!replay_slow_prerequisites())
		return false;
	const bool shift_held =
		*(const uint8_t*)game_address(kThpracShiftHeldRva) != 0;
	const bool trigger_held =
		*(const uint8_t*)game_address(kThpracFarTriggerHeldRva) != 0;
	return (live_input_state & kInputFocusMask) != 0 ||
		shift_held || trigger_held;
}

void reflect_focus_for_thprac() {
	if (!Config::ReplaySpeedControl || !thprac_loaded()) {
		return;
	}
	const bool focus_held = (live_input_state & kInputFocusMask) != 0;
	if (focus_held && replay_slow_prerequisites()) {
		*(uint8_t*)game_address(kThpracShiftHeldRva) = 1;
		thprac_focus_mirrored = true;
	} else if (thprac_focus_mirrored) {
		const bool physical_shift =
			(GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0;
		*(uint8_t*)game_address(kThpracShiftHeldRva) =
			physical_shift ? 1 : 0;
		thprac_focus_mirrored = false;
	}
}

void begin_replay_slow() {
	using AudioPositionFunction = int64_t(__fastcall*)(int);
	const int stream = *(int*)game_address(kAudioStreamRva);
	replay_slow_stream = stream;
	replay_slow_bgm_start = stream >= 0
		? ((AudioPositionFunction)game_address(kAudioPositionRva))(stream)
		: -1;
	replay_slow_frames = 0;
	replay_slow_ticks = 0;
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
	replay_slow_ticks = 0;
	replay_slow_credit = 0.0;
	replay_slow_debt = 0.0;
}

int64_t __fastcall update_chain_hook(void* chain) {
	auto update_chain =
		(UpdateChainFunction)game_address(kUpdateChainRva);

	if (thprac_loaded()) {
		logic_update_ran = true;
		return update_chain(chain);
	}

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
	if (!logic_update_hook_installed) {
		++replay_slow_frames;
		replay_slow_debt = (double)replay_slow_frames *
			(1.0 / (double)X64Config::ReplaySlowRate - 1.0);
	}
	return update_chain(chain);
}

void update_thprac_slow_tracking() {
	if (!thprac_loaded())
		return;
	const bool requested = thprac_replay_slow_requested();
	if (requested && !replay_slow_active)
		begin_replay_slow();
	else if (!requested && replay_slow_active)
		end_replay_slow();
}

int64_t __fastcall update_chain_entry_hook(void* chain) {
	update_thprac_slow_tracking();
	++logic_updates_this_frame;
	++total_logic_updates;
	if (extra_render_active || extra_presentation_active)
		++extra_logic_updates;
	logic_update_ran = true;
	if (replay_slow_active)
		++replay_slow_frames;
	return logic_update_original(chain);
}

bool install_logic_update_hook() {
	if (logic_update_hook_installed)
		return true;
	static const uint8_t expected_prologue[] = {
		0x48, 0x89, 0x5C, 0x24, 0x08,
		0x48, 0x89, 0x6C, 0x24, 0x10,
		0x48, 0x89, 0x74, 0x24, 0x18, 0x57,
	};
	uint8_t* target = game_address(kUpdateChainRva);
	if (memcmp(target, expected_prologue, sizeof(expected_prologue)) != 0)
		return false;
	logic_update_original = (UpdateChainFunction)install_x64_detour(
		target, (void*)&update_chain_entry_hook, sizeof(expected_prologue));
	logic_update_hook_installed = logic_update_original != nullptr;
	return logic_update_hook_installed;
}

void restore_thprac_replay_defaults() {
	uint8_t* compare = game_address(kReplayFastLoopCompareRva);
	uint8_t* branch = game_address(kReplayFastLoopBranchRva);
	if (compare[0] == 0x83 && compare[1] == 0xFF &&
		compare[3] == 0x7C && compare[4] == 0xB3) {
		const uint8_t default_extra_updates = 7;
		patch_bytes(game_address(kReplayFastLoopImmediateRva),
			(void*)&default_extra_updates, sizeof(default_extra_updates));
	}
	if (branch[0] == 0xEB && branch[1] == 0x3C) {
		const uint8_t normal_branch = 0x74;
		patch_bytes(branch, (void*)&normal_branch, sizeof(normal_branch));
	}
}

void synchronize_thprac_compatibility() {
	if (!thprac_loaded() || thprac_compatibility_ready)
		return;
	restore_replay_rate_text();
	if (replay_fast_rate_hook_installed) {
		restore_thprac_replay_defaults();
		replay_fast_rate_hook_installed = false;
	}
	const bool logic_hook = install_logic_update_hook();
	thprac_compatibility_ready = true;
	printf("TH06NC: thprac compatibility active, logic update hook %d\n",
		logic_hook ? 1 : 0);
}

UINT thprac_external_game_fps() {
	if (!thprac_loaded())
		return 0;
	const uint64_t period =
		*(const uint64_t*)game_address(kFpsLimiterConstantRva);
	if (period == 0)
		return 0;
	LARGE_INTEGER frequency;
	if (!QueryPerformanceFrequency(&frequency) ||
		frequency.QuadPart <= 0) {
		return 0;
	}
	const uint64_t fps =
		((uint64_t)frequency.QuadPart + period / 2) / period;
	return fps > 0 && fps <= 1000 ? (UINT)fps : 0;
}

double interpolation_present_rate() {
	if (X64Config::InterpolationFPS != 0)
		return (double)X64Config::InterpolationFPS;
	if (Limiter::game_fps <= 0.0)
		return 0.0;

	if (X64Config::Interpolation == 1) {
		return (double)Limiter::DisplayRefresh();
	}

	int multiplier = X64Config::Interpolation;
	if (multiplier < 0) {
		const UINT display = Limiter::DisplayRefresh();
		const UINT logic = (UINT)(Limiter::game_fps + 0.5);
		if (display == 0 || logic == 0)
			return 0.0;
		multiplier = (int)(display / logic);
	}
	if (multiplier < 2)
		return 0.0;
	return Limiter::game_fps * (double)multiplier;
}

bool interpolation_eligible() {
	if (!frame_compose_hook_installed ||
		(X64Config::Interpolation == 0 &&
			X64Config::InterpolationFPS == 0)) {
		return false;
	}
	if (current_frame_updates > 1 ||
		(Limiter::DisplayRefresh() != 0 &&
			Limiter::game_fps > Limiter::DisplayRefresh())) {
		return false;
	}
	return true;
}

unsigned int next_extra_presentations(LARGE_INTEGER& spacing) {
	spacing.QuadPart = 0;
	const double rate = interpolation_present_rate();
	if (!interpolation_eligible() || rate <= 0.0) {
		interpolation_presentation_owed = 0.0;
		return 0;
	}

	interpolation_presentation_owed +=
		rate / Limiter::game_fps - 1.0;
	unsigned int extras = 0;
	while (interpolation_presentation_owed >= 1.0 - 1e-9 &&
		extras < kMaxExtraPresentations) {
		interpolation_presentation_owed -= 1.0;
		++extras;
	}
	if (extras == 0)
		return 0;

	spacing.QuadPart = Limiter::FrameWait().QuadPart /
		(LONGLONG)(extras + 1);
	return spacing.QuadPart > 0 ? extras : 0;
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
		"ForceFlipDiscard %d, interpolation mode %d, "
		"fixed rate %u, EnemyBullets %d, Lasers %d, PlayerBody %d, "
		"PlayerShots %d, AllowTearing %d\n",
		Config::GameFPS, Config::ReplaySpeedControl ? 1 : 0,
		X64Config::ReplaySlowRate, X64Config::ReplayFastRate,
		Config::ShowOverlay,
		Config::DebugConsole ? 1 : 0,
		X64Config::ForceFlipDiscard ? 1 : 0,
		X64Config::Interpolation, X64Config::InterpolationFPS,
		X64Config::InterpolateEnemyBullets ? 1 : 0,
		X64Config::InterpolateLasers ? 1 : 0,
		X64Config::InterpolatePlayerBody ? 1 : 0,
		X64Config::InterpolatePlayerShots ? 1 : 0,
		X64Config::AllowTearing ? 1 : 0);
	fprintf(file,
		"swap chain: FLIP_DISCARD hook %d, current FLIP_DISCARD %d, "
		"current tearing %d\n",
		flip_discard_hook_installed ? 1 : 0,
		d3d11_swapchain_is_flip_discard() ? 1 : 0,
		d3d11_swapchain_allows_tearing() ? 1 : 0);
	fprintf(file,
		"hooks: replay indicator %d, update chain %d, "
		"input update %d, replay fast rate %d, logic update %d, "
		"frame compose %d\n",
		replay_indicator_hook_installed ? 1 : 0,
		update_chain_hook_installed ? 1 : 0,
		input_update_hook_installed ? 1 : 0,
		replay_fast_rate_hook_installed ? 1 : 0,
		logic_update_hook_installed ? 1 : 0,
		frame_compose_hook_installed ? 1 : 0);
	fprintf(file, "practice tool module: %d\n",
		practice_tool_loaded ? 1 : 0);
	fprintf(file,
		"thprac module: %d, compatibility %d, external FPS %u, "
		"replay fast x%u, UI replay %d\n",
		thprac_loaded() ? 1 : 0, thprac_compatibility_ready ? 1 : 0,
		thprac_external_game_fps(),
		(unsigned int)*(const uint8_t*)game_address(
			kReplayFastLoopImmediateRva) + 1,
		thprac_game_gui_progress != nullptr ? 1 : 0);
	fprintf(file,
		"game state: mode %u (%s), input 0x%08lx, "
		"live input 0x%08lX, focus %d, shoot %d, "
		"replay controller 0x%p, replay active %d, slow active %d, "
		"slow updates %lld, BGM debt %.2f frame(s), "
		"replay blocked %d, enemy updates blocked %d\n",
		game_mode, game_mode_name(game_mode), (unsigned long)input,
		(unsigned long)live_input_state,
		(live_input_state & kInputFocusMask) != 0 ? 1 : 0,
		(live_input_state & kInputShootMask) != 0 ? 1 : 0,
		replay_controller, replay_is_active(replay_controller) ? 1 : 0,
		replay_slow_active ? 1 : 0,
		(long long)replay_slow_frames, replay_slow_debt,
		*(const uint8_t*)game_address(kReplaySpeedBlockedRva) != 0
			? 1 : 0,
		*(const uint8_t*)game_address(kBulletUpdateBlockedRva) != 0
			? 1 : 0);
	fprintf(file, "last slow BGM seek: %lld -> %lld\n",
		(long long)replay_last_bgm_position,
		(long long)replay_last_bgm_target);
	fprintf(file, "last update ran: %d\n",
		logic_update_ran ? 1 : 0);
	LARGE_INTEGER logic_now;
	QueryPerformanceCounter(&logic_now);
	const double logic_seconds = logic_measure_start.QuadPart != 0
		? (double)(logic_now.QuadPart - logic_measure_start.QuadPart) /
			(double)diag_frequency.QuadPart
		: 0.0;
	uint64_t recorded_updates = 0;
	int64_t recorded_first_present = 0;
	int64_t recorded_last_present = 0;
	for (unsigned int i = 0; i < count; ++i) {
		const DiagFrame& frame =
			diag_frames[(start + i) % kDiagFrameCapacity];
		recorded_updates += frame.updates;
		if (i == 0)
			recorded_first_present = frame.present.QuadPart;
		recorded_last_present = frame.present.QuadPart;
	}
	const double recorded_seconds =
		(double)(recorded_last_present - recorded_first_present) /
		(double)diag_frequency.QuadPart;
	const double recorded_logic_fps = recorded_seconds > 0.0
		? (double)recorded_updates / recorded_seconds
		: 0.0;
	fprintf(file,
		"logic updates: total %llu, session %.3f fps, "
		"recorded window %.3f fps with %llu updates, "
		"during extra render/compose %llu\n",
		(unsigned long long)total_logic_updates,
		logic_seconds > 0.0
			? (double)total_logic_updates / logic_seconds
			: 0.0,
		recorded_logic_fps,
		(unsigned long long)recorded_updates,
		(unsigned long long)extra_logic_updates);
	char native_fps_text[257] = {};
	memcpy(native_fps_text, game_address(kNativeFpsTextRva),
		sizeof(native_fps_text) - 1);
	fprintf(file,
		"native FPS display: text \"%s\", counter %d, "
		"window start %lld\n",
		native_fps_text,
		*(const int32_t*)game_address(kNativeFpsCounterRva),
		(long long)*(const int64_t*)game_address(
			kNativeFpsWindowStartRva));
	fprintf(file,
		"interpolation: running %d, rate %.3f, extras total %llu, "
		"state snapshot %d, owed %.3f, window foreground %d\n",
		interpolation_eligible() ? 1 : 0,
		interpolation_present_rate(),
		(unsigned long long)interpolation_extra_count,
		logic_state_snapshot != nullptr ? 1 : 0,
		interpolation_presentation_owed,
		game_foreground ? 1 : 0);
	fprintf(file, "player: position %.3f,%.3f\n",
		*(const float*)(player + kPlayerX),
		*(const float*)(player + kPlayerY));
	const PlayerInterpolation::Step predicted =
		predicted_player_step();
	fprintf(file,
		"player prediction: state %u, input 0x%08lx, "
		"measured %.4f,%.4f, predicted %.4f,%.4f, "
		"speed %.4f/%.4f diagonal %.4f/%.4f multiplier %.4f/%.4f, "
		"bounds x %.3f..%.3f y %.3f..%.3f\n",
		player_diagnostic.state,
		(unsigned long)player_diagnostic.input,
		player_diagnostic.measured_x,
		player_diagnostic.measured_y,
		predicted.x, predicted.y,
		player_diagnostic.speed,
		player_diagnostic.focus_speed,
		player_diagnostic.diagonal,
		player_diagnostic.focus_diagonal,
		player_diagnostic.multiplier_x,
		player_diagnostic.multiplier_y,
		player_diagnostic.min_x,
		player_diagnostic.max_x,
		player_diagnostic.min_y,
		player_diagnostic.max_y);
	for (unsigned int option = 0; option < 2; ++option) {
		fprintf(file,
			"player option %u: position %.3f,%.3f, "
			"measured %.4f,%.4f\n",
			option,
			player_diagnostic.option_x[option],
			player_diagnostic.option_y[option],
			player_diagnostic.option_step_x[option],
			player_diagnostic.option_step_y[option]);
	}
	fprintf(file,
		"interpolated objects: enemy bullets %u, lasers %u, "
		"player %d, player shots %u, laser angle jumps %u\n",
		current_frame_bullets_projected,
		current_frame_lasers_projected,
		current_frame_player_projected ? 1 : 0,
		current_frame_player_shots_projected,
		laser_angle_jumps);
	const uint8_t* shots = player + kPlayerShotsOffset;
	for (unsigned int i = 0; i < kPlayerShotCount; ++i) {
		const uint8_t* shot =
			shots + (size_t)i * kPlayerShotSize;
		const uint16_t state = *(const uint16_t*)(
			shot + kPlayerShotStateOffset);
		if (state == 0)
			continue;
		const uint16_t type = *(const uint16_t*)(
			shot + kPlayerShotTypeOffset);
		const float* draw = (const float*)(
			shot + kPlayerShotDrawPositionOffset);
		const float* logical = (const float*)(
			shot + kPlayerShotLogicalPositionOffset);
		const MeasuredStep& motion = player_shot_motion[i].step;
		fprintf(file,
			"player shot %u: state %u, type %u, draw %.3f,%.3f, "
			"logical %.3f,%.3f, step %.4f,%.4f\n",
			i, state, type, draw[0], draw[1],
			logical[0], logical[1],
			motion.step_x, motion.step_y);
	}
	for (unsigned int i = 0; i < kLaserSlotCount; ++i) {
		const uint8_t* slot =
			game_address(kLaserSlotsRva) +
			(size_t)i * kLaserSlotSize;
		if (*(const uint8_t*)(slot + kLaserActiveOffset) == 0)
			continue;
		float values[kLaserFieldCount];
		read_laser_fields(slot, values);
		const LaserMotion& motion = laser_motion[i];
		fprintf(file,
			"laser %u: state %u, sprite %u, "
			"pos %.3f,%.3f, angle %.4f, start %.3f, end %.3f, "
			"width %.3f, step %.4f,%.4f,%.4f,%.4f,%.4f,%.4f\n",
			i,
			(unsigned int)*(const uint8_t*)(slot + kLaserStateOffset),
			(unsigned int)*(const uint16_t*)(slot + kLaserSpriteOffset),
			values[0], values[1], values[2],
			values[3], values[4], values[5],
			motion.step[0], motion.step[1], motion.step[2],
			motion.step[3], motion.step[4], motion.step[5]);
	}
	fprintf(file, "frame work %.3fms, frame wait %.3fms\n",
		ticks_to_ms((uint64_t)Limiter::FrameWork().QuadPart),
		ticks_to_ms((uint64_t)Limiter::FrameWait().QuadPart));
	fprintf(file, "frames recorded: %llu, showing %u\n",
		(unsigned long long)diag_frame_count, count);

	for (unsigned int i = 0; i < count; ++i) {
		const DiagFrame& frame =
			diag_frames[(start + i) % kDiagFrameCapacity];
		fprintf(file,
			"frame %4u: mode %u, updates %u, update ran %d, "
			"slow %d, extras %u/%u, bullets %u, lasers %u, shots %u, "
			"player %d, restores %u, "
			"present %.3fms\n",
			i, frame.game_mode, frame.updates,
			frame.update_ran ? 1 : 0, frame.replay_slow ? 1 : 0,
			frame.extras_presented,
			frame.extras_requested, frame.bullets_projected,
			frame.lasers_projected,
			frame.player_shots_projected,
			frame.player_projected ? 1 : 0,
			frame.state_restores,
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
	synchronize_thprac_compatibility();
	update_thprac_slow_tracking();
	discover_thprac_interpolation_state();
	update_foreground_state();
	if (logic_update_hook_installed) {
		current_frame_updates =
			(uint32_t)(logic_updates_this_frame > 0xFFFFFFFFull
				? 0xFFFFFFFFull : logic_updates_this_frame);
		logic_update_ran = logic_updates_this_frame != 0;
		logic_updates_this_frame = 0;
	} else {
		current_frame_updates = logic_update_ran ? 1 : 0;
	}
	if (replay_slow_active) {
		++replay_slow_ticks;
		if (replay_slow_ticks > replay_slow_frames) {
			replay_slow_debt =
				(double)(replay_slow_ticks - replay_slow_frames);
		}
	}
	current_frame_extras_requested = 0;
	current_frame_extras_presented = 0;
	current_frame_bullets_projected = 0;
	current_frame_lasers_projected = 0;
	current_frame_player_shots_projected = 0;
	current_frame_player_projected = false;
	current_frame_state_restores = 0;
	Limiter::Tick();
	if (current_frame_updates == 1)
		measure_player_and_shots();
	else
		reset_player_measurements();
	if (current_frame_updates == 1)
		measure_lasers();
	else
		reset_laser_measurements();
	render_chain();
	return 0;
}

uintptr_t frame_finish() {
	if (extra_presentation_active) {
		const uintptr_t result = screen_flip();
		++current_frame_extras_presented;
		return result;
	}

	QueryPerformanceCounter(&normal_presentation_base);
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
	frame.updates = current_frame_updates;
	frame.extras_requested = 0;
	frame.extras_presented = 0;
	frame.bullets_projected = 0;
	frame.lasers_projected = 0;
	frame.player_shots_projected = 0;
	frame.state_restores = 0;
	frame.player_projected = false;
	frame.update_ran = logic_update_ran;
	frame.replay_slow = replay_slow_active;
	++diag_frame_count;
	check_diagnostic_hotkey();
	return result;
}

uintptr_t __fastcall frame_compose_hook() {
	if (frame_compose_original == nullptr)
		return 0;
	LARGE_INTEGER normal_end;
	const uintptr_t result = frame_compose_original();
	QueryPerformanceCounter(&normal_end);

	LARGE_INTEGER spacing;
	const uint32_t extras = next_extra_presentations(spacing);
	current_frame_extras_requested = extras;
	if (extras == 0)
		return result;
	if (!ensure_logic_state_snapshot()) {
		current_frame_extras_requested = 0;
		printf("TH06NC: logic-state snapshot allocation failed\n");
		return result;
	}
	save_logic_state_snapshot();

	const LARGE_INTEGER frame_start = Limiter::FrameStart();
	const LARGE_INTEGER frame_wait = Limiter::FrameWait();
	const LARGE_INTEGER base = normal_presentation_base.QuadPart != 0
		? normal_presentation_base
		: normal_end;
	LARGE_INTEGER game_work;
	game_work.QuadPart = frame_start.QuadPart != 0
		? base.QuadPart - frame_start.QuadPart
		: 0;
	LARGE_INTEGER deadline;
	deadline.QuadPart = frame_start.QuadPart + frame_wait.QuadPart;
	const bool has_deadline =
		frame_start.QuadPart != 0 && game_work.QuadPart > 0;

	for (uint32_t k = 1; k <= extras; ++k) {
		LARGE_INTEGER target;
		target.QuadPart = base.QuadPart +
			spacing.QuadPart * (LONGLONG)k;
		if (has_deadline &&
			target.QuadPart + game_work.QuadPart > deadline.QuadPart) {
			continue;
		}

		Limiter::WaitUntil(target);
		const float t = (float)k / (float)(extras + 1);
		const bool enemy_blocked = enemy_object_updates_blocked();
		if (X64Config::InterpolateEnemyBullets && !enemy_blocked)
			advance_flying_bullets(t);
		if (X64Config::InterpolatePlayerBody)
			advance_player_and_options(t);
		if (X64Config::InterpolatePlayerShots)
			advance_player_shots(t);
		if (X64Config::InterpolateLasers && !enemy_blocked)
			advance_lasers(t);
		extra_render_active = true;
		render_chain();
		extra_render_active = false;
		if (X64Config::InterpolateLasers)
			restore_lasers();
		if (X64Config::InterpolatePlayerShots)
			restore_player_shots();
		if (X64Config::InterpolatePlayerBody)
			restore_player_and_options();
		restore_flying_bullets();
		prepare_thprac_for_extra_presentation();

		NativeFpsState native_fps;
		save_native_fps_state(&native_fps);
		extra_presentation_active = true;
		frame_compose_original();
		extra_presentation_active = false;
		restore_native_fps_state(&native_fps);
		restore_logic_state_if_changed();
		++interpolation_extra_count;
	}

	if (diag_frame_count != 0) {
		DiagFrame& frame = diag_frames[
			(size_t)((diag_frame_count - 1) % kDiagFrameCapacity)];
		frame.extras_requested = current_frame_extras_requested;
		frame.extras_presented = current_frame_extras_presented;
		frame.bullets_projected = current_frame_bullets_projected;
		frame.lasers_projected = current_frame_lasers_projected;
		frame.player_shots_projected =
			current_frame_player_shots_projected;
		frame.player_projected = current_frame_player_projected;
		frame.state_restores = current_frame_state_restores;
	}
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
	Limiter::external_game_fps = thprac_external_game_fps;
	QueryPerformanceFrequency(&diag_frequency);
	QueryPerformanceCounter(&logic_measure_start);
	const bool thprac_present = thprac_loaded();
	discover_thprac_interpolation_state();

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

	if (!thprac_present) {
		replay_fast_rate_hook_installed = install_replay_fast_rate_patch();
		if (!replay_fast_rate_hook_installed) {
			printf("TH06NC: replay fast-forward rate patch at RVA %p does not match\n",
				(void*)kReplayFastLoopCompareRva);
		}
	}

	if (!thprac_present && install_replay_rate_text_patch()) {
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
	if (!install_logic_update_hook()) {
		printf("TH06NC: logic update entry hook at RVA 0x%llX "
			"does not match\n", (unsigned long long)kUpdateChainRva);
	}

	uint8_t* finish_call = game_address(kFrameFinishCallRva);
	if (call_targets(finish_call, game_base + kScreenFlipRva)) {
		patch_call(finish_call, (void*)&frame_finish);
	} else {
		printf("TH06NC: ScreenFlip call at RVA %p does not match\n",
			(void*)kFrameFinishCallRva);
	}

	uint8_t* compose_call = game_address(kFrameComposeCallRva);
	if (call_targets(compose_call, game_base + kFrameComposeRva)) {
		frame_compose_original =
			(FrameComposeFunction)game_address(kFrameComposeRva);
		patch_call(compose_call, (void*)&frame_compose_hook);
		frame_compose_hook_installed = true;
	} else {
		printf("TH06NC: frame compose call at RVA %p does not match\n",
			(void*)kFrameComposeCallRva);
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

	printf("TH06NC: x64 runtime entry installed, interpolation %d/%u, "
		"enemy bullets %d\n", X64Config::Interpolation,
		X64Config::InterpolationFPS,
		X64Config::InterpolateEnemyBullets ? 1 : 0);
	synchronize_thprac_compatibility();
}

FPSTarget th06nc_replay_callback() {
	return FPSTarget::Game;
}
