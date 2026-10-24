#include <Windows.h>
#include <shlwapi.h>
#include <stdio.h>
#include <string.h>
#include "config.h"
#include "default_ini.h"

// Default config values
UINT Config::GameFPS = 60;
BOOL Config::ReplaySpeedControl = TRUE;
UINT Config::ReplaySkipFPS = 240;
UINT Config::ReplaySlowFPS = 30;
UINT Config::BltPrepareTime = 2;
SleepType Config::Sleep = SleepType::Vpatch;
BOOL Config::D3D9Ex = TRUE;
TargetRefreshRate Config::FullscreenRefreshRate = TargetRefreshRate::Max;
int Config::ShowOverlay = 1;
BOOL Config::DebugConsole = FALSE;
BOOL Config::DebugWait = FALSE;
BOOL Config::FixInputGlitching = TRUE;
BOOL Config::CalcFPS = TRUE;
int Config::Interpolation = 1;
UINT Config::InterpolationFPS = 0;
BOOL Config::InterpolatePlayerBody = TRUE;
BOOL Config::InterpolatePlayerShots = TRUE;
BOOL Config::InterpolateBulletsAndLasers = TRUE;
BOOL Config::AlwaysBlt = TRUE;
TouhouGame Config::GameOverride = TouhouGame::Unknown;
BOOL Config::AskWindowMode = FALSE;
BOOL Config::WindowEnabled = FALSE;
UINT Config::WindowX = CW_USEDEFAULT;
UINT Config::WindowY = CW_USEDEFAULT;
UINT Config::WindowWidth = 640;
UINT Config::WindowHeight = 480;
BOOL Config::WindowTitleBar = TRUE;
BOOL Config::WindowAlwaysOnTop = FALSE;
int Config::WindowCoverTaskbar = 0;

// The file written to %APPDATA%\ShanghaiAlice\oilp_custom\openinputlagpatch.ini the first time the
// patch runs. It has the same keys and defaults as the shipped openinputlagpatch.ini; the README
// documents what each one means. A game folder's own openinputlagpatch.ini is loaded afterwards and
// overrides any key it contains.
static const char default_global_ini[] =
    "; OpenInputLagPatch global settings.\r\n"
    "; This file is read before the game folder's openinputlagpatch.ini.\r\n"
    "; If the game folder has its own openinputlagpatch.ini, that file overrides these values.\r\n"
    "\r\n"
    "[Interpolation]\r\n"
    "Interpolation = 1\r\n"
    "PlayerBody = 1\r\n"
    "PlayerShots = 1\r\n"
    "BulletsAndLasers = 1\r\n"
    "\r\n"
    "[Option]\r\n"
    "GameFPS = 60\r\n"
    "ReplaySpeedControl = 1\r\n"
    "ReplaySkipFPS = 240\r\n"
    "ReplaySlowFPS = 30\r\n"
    "BltPrepareTime = 2\r\n"
    "Sleep = 1\r\n"
    "D3D9Ex = 1\r\n"
    "FullscreenRefreshRate = 0\r\n"
    "ShowOverlay = 1\r\n"
    "DebugConsole = 0\r\n"
    "DebugWait = 0\r\n"
    "FixInputGlitching = 1\r\n"
    "CalcFPS = 1\r\n"
    "AlwaysBlt = 1\r\n"
    "GameOverride = -1\r\n"
    "\r\n"
    "[Window]\r\n"
    "AskWindowMode = 0\r\n"
    "enabled = 0\r\n"
    "X = 2147483648\r\n"
    "Y = 2147483648\r\n"
    "Width = 640\r\n"
    "Height = 480\r\n"
    "TitleBar = 1\r\n"
    "AlwaysOnTop = 0\r\n"
    "CoverTaskbar = 0\r\n";

// Builds the global path and creates the file with the defaults if this is the first run.
static bool ensure_global_config(wchar_t* path, size_t path_count) {
    wchar_t appdata[MAX_PATH] = {};
    DWORD length = GetEnvironmentVariableW(L"APPDATA", appdata, _countof(appdata));
    if (length == 0 || length >= _countof(appdata))
        return false;

    wchar_t directory[MAX_PATH] = {};
    wcscpy_s(directory, appdata);
    PathAppendW(directory, L"ShanghaiAlice");
    CreateDirectoryW(directory, nullptr);
    PathAppendW(directory, L"oilp_custom");
    CreateDirectoryW(directory, nullptr);

    wcscpy_s(path, path_count, directory);
    PathAppendW(path, L"openinputlagpatch.ini");

    if (PathFileExistsW(path))
        return true;

    HANDLE file = CreateFileW(path, GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE)
        return PathFileExistsW(path);

    // The reference ini is embedded as a resource so the generated global file keeps the full
    // bilingual comments and does not depend on the source file's encoding.
    bool wrote = false;
    // FindResource with NULL searches the executable, not this DLL. Get the module that owns this
    // code so the embedded reference ini is found.
    HMODULE module = NULL;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
        (LPCWSTR)&ensure_global_config, &module);
    HRSRC resource = module != NULL
                         ? FindResourceW(module, MAKEINTRESOURCEW(IDR_DEFAULT_INI), RT_RCDATA)
                         : NULL;
    if (resource != NULL) {
        DWORD size = SizeofResource(module, resource);
        HGLOBAL loaded = LoadResource(module, resource);
        const void* data = loaded != NULL ? LockResource(loaded) : nullptr;
        if (data != nullptr && size != 0) {
            DWORD written = 0;
            wrote = WriteFile(file, data, size, &written, nullptr) && written == size;
        }
    }
    if (!wrote) {
        DWORD written = 0;
        WriteFile(file, default_global_ini, (DWORD)strlen(default_global_ini), &written, nullptr);
    }
    CloseHandle(file);
    return true;
}

// Helper macro for loading a specific setting value
#define LOAD_SETTING(x) Config::x = (decltype(Config::x))GetPrivateProfileInt(TEXT("Option"), TEXT(#x), (int)Config::x, config_path)
// Same as LOAD_SETTING, but for the [Window] section (the keys don't match the field names)
#define LOAD_WINDOW_SETTING(x, key) Config::x = (decltype(Config::x))GetPrivateProfileInt(TEXT("Window"), TEXT(key), (int)Config::x, config_path)
// Same as LOAD_SETTING, but for the [Interpolation] section (the keys don't match the field names)
#define LOAD_INTERPOLATION_SETTING(x, key) Config::x = (decltype(Config::x))GetPrivateProfileInt(TEXT("Interpolation"), TEXT(key), (int)Config::x, config_path)

// Loads one ini file over the current values. Missing keys keep the value the previous file (or the
// defaults) left, so the global file can be loaded first and the game folder's file overrides only
// the keys it actually contains.
static void load_config_file(const wchar_t* config_path) {
	// Load the config
	LOAD_SETTING(GameFPS);
	LOAD_SETTING(ReplaySpeedControl);
	LOAD_SETTING(ReplaySkipFPS);
	LOAD_SETTING(ReplaySlowFPS);
	LOAD_SETTING(BltPrepareTime);
	LOAD_SETTING(Sleep);
	LOAD_SETTING(D3D9Ex);
	LOAD_SETTING(FullscreenRefreshRate);
	LOAD_SETTING(ShowOverlay);
	LOAD_SETTING(DebugConsole);
	LOAD_SETTING(DebugWait);
	LOAD_SETTING(FixInputGlitching);
	LOAD_SETTING(CalcFPS);
	// Interpolation is read as text below, it has more than one form
	LOAD_SETTING(AlwaysBlt);
	LOAD_SETTING(GameOverride);

	// Load the [Window] section
	LOAD_WINDOW_SETTING(AskWindowMode, "AskWindowMode");
	LOAD_WINDOW_SETTING(WindowEnabled, "enabled");
	LOAD_WINDOW_SETTING(WindowX, "X");
	LOAD_WINDOW_SETTING(WindowY, "Y");
	LOAD_WINDOW_SETTING(WindowWidth, "Width");
	LOAD_WINDOW_SETTING(WindowHeight, "Height");
	LOAD_WINDOW_SETTING(WindowTitleBar, "TitleBar");
	LOAD_WINDOW_SETTING(WindowAlwaysOnTop, "AlwaysOnTop");
	LOAD_WINDOW_SETTING(WindowCoverTaskbar, "CoverTaskbar");

	// Frame interpolation: "0", "1", "-1", a multiplier, or "*<frame rate>"
	{
		wchar_t value[64] = {};
		GetPrivateProfileStringW(TEXT("Interpolation"), TEXT("Interpolation"), L"\x1", value, 64, config_path);
		// Older files kept the master switch in [Option]; still read it from there when the new
		// section does not have one.
		if (wcscmp(value, L"\x1") == 0)
			GetPrivateProfileStringW(TEXT("Option"), TEXT("Interpolation"), L"\x1", value, 64, config_path);
		// The sentinel means this file has no Interpolation key: keep what the previous file set.
		if (wcscmp(value, L"\x1") != 0) {
			if (value[0] == L'*') {
				Config::InterpolationFPS = (UINT)_wtoi(value + 1);
				Config::Interpolation = 0;
			} else {
				Config::Interpolation = _wtoi(value);
				Config::InterpolationFPS = 0;
			}
		}

		if (Config::Interpolation < -1)
			Config::Interpolation = -1;
		if (Config::InterpolationFPS > 0 && Config::InterpolationFPS < 60)
			Config::InterpolationFPS = 60;
	}

	LOAD_INTERPOLATION_SETTING(InterpolatePlayerBody, "PlayerBody");
	LOAD_INTERPOLATION_SETTING(InterpolatePlayerShots, "PlayerShots");
	LOAD_INTERPOLATION_SETTING(InterpolateBulletsAndLasers, "BulletsAndLasers");
}

bool Config::Load() {
	// The game's own folder is both where its optional ini lives and where Ju.N.Owen would be.
	wchar_t game_dir[1024] = {};
	if (!GetModuleFileNameW(NULL, game_dir, MAX_PATH))
		return false;
	PathRemoveFileSpecW(game_dir);

	wchar_t game_config[1024] = {};
	wcscpy_s(game_config, game_dir);
	PathAppendW(game_config, L"openinputlagpatch.ini");

	// Global settings first, then the game folder's file overrides the keys it contains.
	wchar_t global_config[1024] = {};
	if (ensure_global_config(global_config, _countof(global_config)))
		load_config_file(global_config);
	if (PathFileExistsW(game_config))
		load_config_file(game_config);

	// Ju.N.Owen (th19's netplay tool) is loaded from modules\th19_junowen.dll by its d3d9.dll. Netplay
	// keeps both sides in step frame by frame, and the extra presentations only add work to every
	// frame, so the interpolation is forced off whenever it's installed.
	wchar_t junowen_path[1024] = {};
	wcscpy_s(junowen_path, game_dir);
	PathAppendW(junowen_path, L"modules\\th19_junowen.dll");
	if ((Config::Interpolation || Config::InterpolationFPS) && PathFileExistsW(junowen_path)) {
		Config::Interpolation = 0;
		Config::InterpolationFPS = 0;
	}

	// Validate options
	if (Config::GameFPS < 60)
		Config::GameFPS = 60;
	if (Config::ReplaySkipFPS < 0)
		Config::ReplaySkipFPS = 240;
	if (Config::ReplaySlowFPS < 0)
		Config::ReplaySlowFPS = 30;
	Config::BltPrepareTime = max(0, min(Config::BltPrepareTime, 16));
	if ((int)Config::Sleep > (int)SleepType::Vpatch)
		Config::Sleep = SleepType::Vpatch;
	if ((int)Config::FullscreenRefreshRate > (int)TargetRefreshRate::MultipleOfSixty)
		Config::FullscreenRefreshRate = TargetRefreshRate::MultipleOfSixty;
	if ((int)Config::GameOverride < (int)TouhouGame::Unknown || (int)Config::GameOverride >= (int)TouhouGame::MaxValue)
		Config::GameOverride = TouhouGame::Unknown;
	if (Config::ShowOverlay < 0)
		Config::ShowOverlay = 0;
	else if (Config::ShowOverlay > 3)
		Config::ShowOverlay = 3;

	return true;
}

#undef LOAD_SETTING
#undef LOAD_WINDOW_SETTING
#undef LOAD_INTERPOLATION_SETTING
