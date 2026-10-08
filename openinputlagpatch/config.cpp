#include <Windows.h>
#include <shlwapi.h>
#include "config.h"

// Default config values
UINT Config::GameFPS = 60;
BOOL Config::ReplaySpeedControl = TRUE;
UINT Config::ReplaySkipFPS = 240;
UINT Config::ReplaySlowFPS = 30;
UINT Config::BltPrepareTime = 2;
SleepType Config::Sleep = SleepType::Vpatch;
BOOL Config::D3D9Ex = TRUE;
TargetRefreshRate Config::FullscreenRefreshRate = TargetRefreshRate::MultipleOfSixty;
BOOL Config::ShowOverlay = TRUE;
BOOL Config::DebugConsole = FALSE;
BOOL Config::DebugWait = FALSE;
BOOL Config::FixInputGlitching = FALSE;
int Config::Interpolation = 0;
UINT Config::InterpolationFPS = 0;
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

// Helper macro for loading a specific setting value
#define LOAD_SETTING(x) Config::x = (decltype(Config::x))GetPrivateProfileInt(TEXT("Option"), TEXT(#x), (int)Config::x, config_path)
// Same as LOAD_SETTING, but for the [Window] section (the keys don't match the field names)
#define LOAD_WINDOW_SETTING(x, key) Config::x = (decltype(Config::x))GetPrivateProfileInt(TEXT("Window"), TEXT(key), (int)Config::x, config_path)

bool Config::Load() {
	// Get the config file path
	wchar_t config_path[1024] = {};
	if (!GetModuleFileNameW(NULL, config_path, MAX_PATH))
		return false;
	PathRemoveFileSpecW(config_path);
	PathAppendW(config_path, L"\\openinputlagpatch.ini");

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

	// Frame interpolation: "0", "-1", a multiplier, or "*<frame rate>"
	{
		wchar_t value[64] = {};
		GetPrivateProfileStringW(TEXT("Option"), TEXT("Interpolation"), TEXT("0"), value, 64, config_path);
		if (value[0] == L'*')
			Config::InterpolationFPS = (UINT)_wtoi(value + 1);
		else
			Config::Interpolation = _wtoi(value);

		if (Config::Interpolation < -1)
			Config::Interpolation = -1;
		if (Config::InterpolationFPS > 0 && Config::InterpolationFPS < 60)
			Config::InterpolationFPS = 60;
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

	return true;
}

#undef LOAD_SETTING
#undef LOAD_WINDOW_SETTING
