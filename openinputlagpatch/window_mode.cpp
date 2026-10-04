#include <Windows.h>
#include <stdio.h>
#include <string.h>
#include <wchar.h>
#include "window_mode.h"
#include "config.h"
#include "games.h"
#include "patch_util.h"

namespace {
	typedef HANDLE(WINAPI* CreateMutexA_t)(LPSECURITY_ATTRIBUTES, BOOL, LPCSTR);
	typedef HANDLE(WINAPI* CreateMutexW_t)(LPSECURITY_ATTRIBUTES, BOOL, LPCWSTR);

	CreateMutexA_t CreateMutexA_orig = nullptr;
	CreateMutexW_t CreateMutexW_orig = nullptr;

	WindowMode::Override override_mode = WindowMode::Override::None;
	bool handled = false;
	bool hooked = false;
	TouhouGame game = TouhouGame::Unknown;

	// Touhou 6 to 9.5 have no way of changing the screen mode from inside the game, which is
	// why vpatch (and this) asks on boot. Touhou 10 and up have their own option for it.
	bool needs_window_mode(TouhouGame g) {
		return g == TouhouGame::Th6 || g == TouhouGame::Th7 || g == TouhouGame::Th8 ||
			g == TouhouGame::Th9 || g == TouhouGame::Th95;
	}

	// Address of the "windowed" flag in the game's own config struct (1 = windowed,
	// 0 = fullscreen). Taken from vpatch's window mode question for th07 to th095 and from
	// the th06 decompilation (g_Supervisor.cfg.windowed at 0x6C6D18 + 0x132) for th06; all
	// of them were confirmed by looking at the instructions that read them.
	BYTE* windowed_flag_address(TouhouGame g) {
		switch (g) {
		case TouhouGame::Th6:
			return (BYTE*)0x006C6E4A;
		case TouhouGame::Th7:
			return (BYTE*)0x00575A8A;
		case TouhouGame::Th8:
			return (BYTE*)0x017CE892;
		case TouhouGame::Th9:
			return (BYTE*)0x004B3539;
		case TouhouGame::Th95:
			return (BYTE*)0x004C483B;
		default:
			return nullptr;
		}
	}

	void ask_window_mode() {
		if (handled)
			return;
		handled = true;

		if (!Config::AskWindowMode || !needs_window_mode(game))
			return;

		// Same question vpatch asks. Answering no makes the game run in window mode even if
		// its own config asks for fullscreen, and vice versa.
		int answer = MessageBoxW(
			NULL,
			L"Launch the game in fullscreen mode?\n\n"
			L"Yes: fullscreen, No: window mode",
			L"OpenInputLagPatch",
			MB_YESNO | MB_ICONQUESTION | MB_SETFOREGROUND
		);
		override_mode = answer == IDYES ? WindowMode::Override::Fullscreen : WindowMode::Override::Windowed;
		printf("Window mode: running in %s\n",
			override_mode == WindowMode::Override::Windowed ? "window mode" : "fullscreen");
	}

	// The games create a mutex named "Touhou ... App" to avoid being started twice. Asking
	// while that mutex exists means tools like thprac can still find the game through it.
	bool is_game_mutex(const char* name) {
		return name != nullptr && _strnicmp(name, "Touhou ", 7) == 0;
	}

	void write_windowed_flag();

	HANDLE WINAPI CreateMutexA_hook(LPSECURITY_ATTRIBUTES attributes, BOOL initial_owner, LPCSTR name) {
		auto ret = CreateMutexA_orig(attributes, initial_owner, name);
		if (is_game_mutex(name)) {
			ask_window_mode();
			// Harmless if the game loads its config later and overwrites this; it covers the
			// games whose config is read before the mutex is created
			write_windowed_flag();
		}
		return ret;
	}

	HANDLE WINAPI CreateMutexW_hook(LPSECURITY_ATTRIBUTES attributes, BOOL initial_owner, LPCWSTR name) {
		auto ret = CreateMutexW_orig(attributes, initial_owner, name);
		if (name != nullptr && _wcsnicmp(name, L"Touhou ", 7) == 0) {
			ask_window_mode();
			write_windowed_flag();
		}
		return ret;
	}

	// Makes the game itself pick the mode that was asked for, so its window, its input
	// handling and dgVoodoo2's fullscreen emulation all follow along
	void write_windowed_flag() {
		if (override_mode == WindowMode::Override::None)
			return;

		BYTE* flag = windowed_flag_address(game);
		if (flag == nullptr)
			return;

		BYTE value = override_mode == WindowMode::Override::Windowed ? 1 : 0;
		DWORD old_protect;
		if (VirtualProtect(flag, sizeof(BYTE), PAGE_READWRITE, &old_protect)) {
			*flag = value;
			VirtualProtect(flag, sizeof(BYTE), old_protect, &old_protect);
			printf("Window mode: game's own windowed flag set to %d\n", value);
		}
	}

	bool game_still_agrees() {
		BYTE* flag = windowed_flag_address(game);
		if (flag == nullptr)
			return true;

		BYTE expected = override_mode == WindowMode::Override::Windowed ? 1 : 0;
		return *flag == expected;
	}

	void apply_to_window(HWND hwnd) {
		// vpatch replaces the window's whole style with a bare WS_VISIBLE popup when the
		// title bar is disabled, which also removes the sizing border
		if (!Config::WindowTitleBar)
			SetWindowLongA(hwnd, GWL_STYLE, WS_VISIBLE);

		// The size is the window's size, not the size of its client area, because that's
		// what vpatch writes into the game's CreateWindowEx call
		UINT flags = SWP_NOZORDER | SWP_FRAMECHANGED;
		if (Config::WindowX == CW_USEDEFAULT || Config::WindowY == CW_USEDEFAULT)
			flags |= SWP_NOMOVE;
		if (Config::WindowWidth == 0 || Config::WindowHeight == 0)
			flags |= SWP_NOSIZE;
		SetWindowPos(hwnd, NULL, (int)Config::WindowX, (int)Config::WindowY,
			(int)Config::WindowWidth, (int)Config::WindowHeight, flags);

		if (Config::WindowAlwaysOnTop)
			SetWindowPos(hwnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOSIZE | SWP_NOMOVE);

		printf("Window mode applied: %dx%d at (%d, %d), titlebar: %d, always on top: %d\n",
			(int)Config::WindowWidth, (int)Config::WindowHeight, (int)Config::WindowX,
			(int)Config::WindowY, Config::WindowTitleBar, Config::WindowAlwaysOnTop);
	}
}

void WindowMode::Hook() {
	if (hooked)
		return;
	hooked = true;

	game = detect_game();
	if (!needs_window_mode(game))
		return;

	// The games call CreateMutexA from their own code, so hooking the executable's import
	// table is enough
	CreateMutexA_orig = (CreateMutexA_t)iat_hook(NULL, "kernel32.dll", "CreateMutexA", (void*)&CreateMutexA_hook);
	CreateMutexW_orig = (CreateMutexW_t)iat_hook(NULL, "kernel32.dll", "CreateMutexW", (void*)&CreateMutexW_hook);
}

void WindowMode::AskIfNeeded() {
	ask_window_mode();
}

void WindowMode::OnD3DCreate() {
	// Fallback in case the game creates its mutex in a way the hook above doesn't see
	ask_window_mode();
	write_windowed_flag();
}

WindowMode::Override WindowMode::GetOverride() {
	return override_mode;
}

WindowMode::Override WindowMode::GetEffectiveOverride() {
	if (override_mode == WindowMode::Override::None)
		return WindowMode::Override::None;
	if (!game_still_agrees())
		return WindowMode::Override::None;
	return override_mode;
}

void WindowMode::Apply(HWND hwnd, BOOL windowed) {
	if (!Config::WindowEnabled || hwnd == NULL || !windowed)
		return;

	apply_to_window(hwnd);
}
