#include <Windows.h>
#include <stdio.h>
#include <string.h>
#include <wchar.h>
#include <shobjidl.h>
#include "window_mode.h"
#include "config.h"
#include "games.h"
#include "patch_util.h"

// The taskbar list (see CoverTaskbar below) is a shell object; uuid.lib carries its class id
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "uuid.lib")

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

	// ---------------------------------------------------------------------------
	// Keeping the taskbar out of the way without always holding the window on top
	// ---------------------------------------------------------------------------
	//
	// The taskbar is a topmost window itself, so a window can only paint over it by being
	// topmost as well - that is what the [Window] AlwaysOnTop does, and it is also why that
	// setting keeps the game above every other window. There are two ways to get the taskbar out
	// of the way without going that far, and the CoverTaskbar option picks between them:
	//
	//   1  tell the shell the window is a fullscreen one (ITaskbarList2::MarkFullscreenWindow),
	//      which makes it step the taskbar aside while the game is the active window. The window
	//      stays a normal window, so whatever is switched to still covers it.
	//   2  hold the window on top only while it is the active window. While playing that is the
	//      same picture as AlwaysOnTop; as soon as another window is activated the game leaves
	//      the topmost band again and both that window and the taskbar are above it.
	//
	// Neither can mean anything while another window is in front - a window that isn't topmost
	// is below the taskbar - which is why both are tied to the window being activated and
	// deactivated. 1 falls back to what 2 does when the taskbar list can't be reached, since the
	// point of the setting is the taskbar not being in the way.
	//
	// The window this is about is the game's own top level one, and it is only ever touched from
	// the watcher thread below, never from the game's own.

	ITaskbarList2* cover_taskbar_list = nullptr;
	bool cover_taskbar_list_asked = false;
	HWND cover_taskbar_window = nullptr;
	bool cover_taskbar_watching = false;
	// What the watcher last worked out, so the window is only touched when it changes
	bool cover_taskbar_active = false;

	ITaskbarList2* get_taskbar_list() {
		if (cover_taskbar_list_asked)
			return cover_taskbar_list;
		cover_taskbar_list_asked = true;

		// The game usually has COM up already, and asking again only returns S_FALSE then
		// (RPC_E_CHANGED_MODE means somebody else picked the other apartment, which this object
		// works in just as well)
		HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
		if (FAILED(hr) && hr != RPC_E_CHANGED_MODE) {
			printf("Window mode: CoInitializeEx failed (0x%lx), the taskbar will be handled by "
				"holding the window on top while it's active\n", (unsigned long)hr);
			return nullptr;
		}

		ITaskbarList2* list = nullptr;
		hr = CoCreateInstance(CLSID_TaskbarList, nullptr, CLSCTX_INPROC_SERVER, IID_ITaskbarList2,
			(void**)&list);
		if (FAILED(hr) || list == nullptr) {
			printf("Window mode: the taskbar list isn't available (0x%lx), the taskbar will be "
				"handled by holding the window on top while it's active\n", (unsigned long)hr);
			return nullptr;
		}
		if (FAILED(list->HrInit())) {
			printf("Window mode: the taskbar list didn't initialize, the taskbar will be handled "
				"by holding the window on top while it's active\n");
			list->Release();
			return nullptr;
		}

		cover_taskbar_list = list;
		return cover_taskbar_list;
	}

	// The one thing that is actually done to the window: while it is the active one, either the
	// shell is told it is a fullscreen window, or it is put into the topmost band. Both are only
	// touched when the answer changes, so whatever reacts to them (the shell, the window's own
	// messages) never gets something to bounce back and forth from.
	void apply_cover_state(bool active) {
		HWND hwnd = cover_taskbar_window;
		if (hwnd == nullptr || active == cover_taskbar_active)
			return;
		cover_taskbar_active = active;

		if (Config::WindowCoverTaskbar == 1) {
			ITaskbarList2* list = get_taskbar_list();
			if (list != nullptr) {
				list->MarkFullscreenWindow(hwnd, active ? TRUE : FALSE);
				return;
			}
			// No taskbar list - fall through to the way that doesn't need the shell
		}

		// Set while the window is active and cleared when it isn't: with it active nothing but
		// another topmost window can be in front of it anyway, and without it the window drops
		// out of the topmost band, so a window switched to covers it again
		SetWindowPos(hwnd, active ? HWND_TOPMOST : HWND_NOTOPMOST, 0, 0, 0, 0,
			SWP_NOSIZE | SWP_NOMOVE | SWP_NOACTIVATE);
	}

	// Watches which window is the active one and keeps the window's state in step with it.
	//
	// This runs on a thread of its own instead of in the game's window procedure, and that is the
	// point: taking the window over means the messages the changes send come back into the same
	// procedure, so that version could re-enter itself and leave the game stuck before it had
	// even shown a picture. Asking the shell for its taskbar object can block as well, and on the
	// game's thread that is just as fatal. Nothing here is done on the game's thread, and the
	// only things it waits for are its own timer and the window surviving.
	DWORD WINAPI cover_taskbar_thread(LPVOID) {
		HWND watched = nullptr;

		for (;;) {
			// The window can be replaced when the game resets its device, and it isn't worth
			// touching before it is actually up
			if (cover_taskbar_window != watched) {
				watched = cover_taskbar_window;
				cover_taskbar_active = false;
			}
			if (watched == nullptr || !IsWindow(watched))
				return 0;
			if (IsWindowVisible(watched))
				apply_cover_state(GetForegroundWindow() == watched);

			Sleep(50);
		}
	}

	void start_cover_taskbar(HWND hwnd) {
		// The window the taskbar can be in the way of is the top level one
		HWND root = GetAncestor(hwnd, GA_ROOT);
		cover_taskbar_window = root != nullptr ? root : hwnd;

		if (cover_taskbar_watching)
			return;
		cover_taskbar_watching = true;

		HANDLE thread = CreateThread(nullptr, 0, cover_taskbar_thread, nullptr, 0, nullptr);
		if (thread != nullptr)
			CloseHandle(thread);
		else
			printf("Window mode: couldn't start the CoverTaskbar watcher (error %lu)\n",
				(unsigned long)GetLastError());
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
		else if (Config::WindowCoverTaskbar)
			start_cover_taskbar(hwnd);

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
