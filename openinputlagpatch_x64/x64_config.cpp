#include <Windows.h>
#include <shlwapi.h>
#include <stdio.h>
#include "config.h"
#include "default_ini.h"

UINT Config::GameFPS = 60;
BOOL Config::ReplaySpeedControl = TRUE;
UINT Config::ReplaySkipFPS = 240;
UINT Config::ReplaySlowFPS = 30;
float Config::ReplaySlowRate = 0.5f;
UINT Config::ReplayFastRate = 8;
UINT Config::BltPrepareTime = 2;
SleepType Config::Sleep = SleepType::Vpatch;
int Config::ShowOverlay = 1;
BOOL Config::DebugConsole = FALSE;
BOOL Config::ForceFlipDiscard = TRUE;

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
	Config::ReplaySlowRate = (float)_wtof(replay_slow_rate);
	Config::ReplayFastRate = GetPrivateProfileIntW(
		L"Option", L"ReplayFastRate", Config::ReplayFastRate, config_path);
	Config::ShowOverlay = GetPrivateProfileIntW(
		L"Option", L"ShowOverlay", Config::ShowOverlay, config_path);
	Config::DebugConsole = GetPrivateProfileIntW(
		L"Option", L"DebugConsole", Config::DebugConsole, config_path);
	Config::ForceFlipDiscard = GetPrivateProfileIntW(
		L"Option", L"ForceFlipDiscard", Config::ForceFlipDiscard, config_path);
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
	if (Config::ReplaySlowRate <= 0.0f || Config::ReplaySlowRate >= 1.0f)
		Config::ReplaySlowRate = 0.5f;
	if (Config::ReplayFastRate < 1)
		Config::ReplayFastRate = 1;
	else if (Config::ReplayFastRate > 128)
		Config::ReplayFastRate = 128;
	Config::ReplaySlowFPS =
		(UINT)((float)Config::GameFPS * Config::ReplaySlowRate + 0.5f);
	if (Config::ReplaySlowFPS < 1)
		Config::ReplaySlowFPS = 1;
	if (Config::ShowOverlay < 0)
		Config::ShowOverlay = 0;
	else if (Config::ShowOverlay > 3)
		Config::ShowOverlay = 3;
	return true;
}
