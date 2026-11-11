#include <Windows.h>
#include <shlwapi.h>
#include <stdio.h>
#include "config.h"
#include "default_ini.h"
#include "x64_config.h"

int Config::GameFPS = 60;
BOOL Config::ReplaySpeedControl = TRUE;
int Config::ReplaySkipFPS = 480;
int Config::ReplaySlowFPS = 30;
UINT Config::BltPrepareTime = 2;
SleepType Config::Sleep = SleepType::Vpatch;
int Config::ShowOverlay = 1;
BOOL Config::DebugConsole = FALSE;

float X64Config::ReplaySlowRate = 0.5f;
UINT X64Config::ReplayFastRate = 8;
BOOL X64Config::ForceFlipDiscard = TRUE;
BOOL X64Config::AllowTearing = TRUE;
int X64Config::Interpolation = 0;
UINT X64Config::InterpolationFPS = 0;
BOOL X64Config::InterpolateEnemyBullets = TRUE;
BOOL X64Config::InterpolateLasers = TRUE;
BOOL X64Config::InterpolatePlayerBody = TRUE;
BOOL X64Config::InterpolatePlayerShots = TRUE;

namespace {
const wchar_t* const config_name = L"openinputlagpatch_x64.ini";

bool ensure_global_config(wchar_t* path, size_t path_count) {
	wchar_t appdata[MAX_PATH] = {};
	const DWORD length = GetEnvironmentVariableW(L"APPDATA", appdata, _countof(appdata));
	if (length == 0 || length >= _countof(appdata))
		return false;

	wchar_t directory[MAX_PATH] = {};
	wcscpy_s(directory, appdata);
	PathAppendW(directory, L"ShanghaiAlice");
	CreateDirectoryW(directory, nullptr);
	PathAppendW(directory, L"oilp_custom");
	CreateDirectoryW(directory, nullptr);

	wcscpy_s(path, path_count, directory);
	PathAppendW(path, config_name);
	if (PathFileExistsW(path))
		return true;

	HANDLE file = CreateFileW(path, GENERIC_WRITE, 0, nullptr, CREATE_NEW,
		FILE_ATTRIBUTE_NORMAL, nullptr);
	if (file == INVALID_HANDLE_VALUE)
		return PathFileExistsW(path);

	HMODULE module = nullptr;
	GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
		GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
		(LPCWSTR)&ensure_global_config, &module);
	HRSRC resource = module != nullptr
		? FindResourceW(module, MAKEINTRESOURCEW(IDR_DEFAULT_INI), RT_RCDATA)
		: nullptr;
	bool wrote = false;
	if (resource != nullptr) {
		const DWORD size = SizeofResource(module, resource);
		HGLOBAL loaded = LoadResource(module, resource);
		const void* data = loaded != nullptr ? LockResource(loaded) : nullptr;
		DWORD written = 0;
		wrote = data != nullptr && size != 0 &&
			WriteFile(file, data, size, &written, nullptr) && written == size;
	}
	CloseHandle(file);
	return wrote || PathFileExistsW(path);
}

void load_config_file(const wchar_t* config_path) {
	Config::GameFPS = GetPrivateProfileIntW(
		L"Option", L"GameFPS", Config::GameFPS, config_path);
	Config::ReplaySpeedControl = GetPrivateProfileIntW(
		L"Option", L"ReplaySpeedControl", Config::ReplaySpeedControl, config_path);
	wchar_t replay_slow_rate[64] = {};
	GetPrivateProfileStringW(L"Option", L"ReplaySlowRate", L"0.5",
		replay_slow_rate, _countof(replay_slow_rate), config_path);
	X64Config::ReplaySlowRate = (float)_wtof(replay_slow_rate);
	X64Config::ReplayFastRate = GetPrivateProfileIntW(
		L"Option", L"ReplayFastRate", X64Config::ReplayFastRate, config_path);
	Config::ShowOverlay = GetPrivateProfileIntW(
		L"Option", L"ShowOverlay", Config::ShowOverlay, config_path);
	Config::DebugConsole = GetPrivateProfileIntW(
		L"Option", L"DebugConsole", Config::DebugConsole, config_path);
	X64Config::ForceFlipDiscard = GetPrivateProfileIntW(
		L"Option", L"ForceFlipDiscard", X64Config::ForceFlipDiscard, config_path);
	X64Config::AllowTearing = GetPrivateProfileIntW(
		L"Option", L"AllowTearing", X64Config::AllowTearing, config_path);
	wchar_t interpolation[64] = {};
	GetPrivateProfileStringW(L"Interpolation", L"Interpolation", L"\x1",
		interpolation, _countof(interpolation), config_path);
	if (interpolation[0] != L'\x1') {
		if (interpolation[0] == L'*') {
			X64Config::Interpolation = 0;
			X64Config::InterpolationFPS = (UINT)_wtoi(interpolation + 1);
		} else {
			X64Config::Interpolation = _wtoi(interpolation);
			X64Config::InterpolationFPS = 0;
		}
	}
	X64Config::InterpolateEnemyBullets = GetPrivateProfileIntW(
		L"Interpolation", L"EnemyBullets",
		X64Config::InterpolateEnemyBullets, config_path);
	X64Config::InterpolateLasers = GetPrivateProfileIntW(
		L"Interpolation", L"Lasers",
		X64Config::InterpolateLasers, config_path);
	X64Config::InterpolatePlayerBody = GetPrivateProfileIntW(
		L"Interpolation", L"PlayerBody",
		X64Config::InterpolatePlayerBody, config_path);
	X64Config::InterpolatePlayerShots = GetPrivateProfileIntW(
		L"Interpolation", L"PlayerShots",
		X64Config::InterpolatePlayerShots, config_path);
}
}

bool Config::Load() {
	wchar_t game_path[1024] = {};
	if (!GetModuleFileNameW(nullptr, game_path, _countof(game_path)))
		return false;
	wchar_t game_dir[1024] = {};
	wcscpy_s(game_dir, game_path);
	PathRemoveFileSpecW(game_dir);

	wchar_t global_config[1024] = {};
	if (ensure_global_config(global_config, _countof(global_config)))
		load_config_file(global_config);

	wchar_t game_config[1024] = {};
	wcscpy_s(game_config, game_dir);
	PathAppendW(game_config, config_name);
	if (PathFileExistsW(game_config))
		load_config_file(game_config);

	if (Config::GameFPS < 60)
		Config::GameFPS = 60;
	if (X64Config::ReplaySlowRate <= 0.0f ||
		X64Config::ReplaySlowRate >= 1.0f) {
		X64Config::ReplaySlowRate = 0.5f;
	}
	if (X64Config::ReplayFastRate < 1)
		X64Config::ReplayFastRate = 1;
	else if (X64Config::ReplayFastRate > 128)
		X64Config::ReplayFastRate = 128;
	Config::ReplaySlowFPS =
		(int)((float)Config::GameFPS * X64Config::ReplaySlowRate + 0.5f);
	if (Config::ReplaySlowFPS < 1)
		Config::ReplaySlowFPS = 1;
	if (Config::ShowOverlay < 0)
		Config::ShowOverlay = 0;
	else if (Config::ShowOverlay > 3)
		Config::ShowOverlay = 3;
	if (X64Config::Interpolation < -1)
		X64Config::Interpolation = -1;
	else if (X64Config::Interpolation > 32)
		X64Config::Interpolation = 32;
	if (X64Config::InterpolationFPS > 0 &&
		X64Config::InterpolationFPS < 60) {
		X64Config::InterpolationFPS = 60;
	} else if (X64Config::InterpolationFPS > 1000) {
		X64Config::InterpolationFPS = 1000;
	}
	X64Config::InterpolateEnemyBullets =
		X64Config::InterpolateEnemyBullets != FALSE;
	X64Config::InterpolateLasers =
		X64Config::InterpolateLasers != FALSE;
	X64Config::InterpolatePlayerBody =
		X64Config::InterpolatePlayerBody != FALSE;
	X64Config::InterpolatePlayerShots =
		X64Config::InterpolatePlayerShots != FALSE;
	X64Config::AllowTearing = X64Config::AllowTearing != FALSE;
	return true;
}
