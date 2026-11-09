#include <Windows.h>
#include <stdio.h>
#include "common.h"
#include "config.h"
#include "limiter.h"
#include "x64_games.h"
#include "x64_overlay.h"

namespace {
void create_console() {
	AllocConsole();
	FILE* stream = nullptr;
	freopen_s(&stream, "CONOUT$", "w", stdout);
	SetConsoleTitleW(L"OpenInputLagPatch x64");
}

void patcher_main() {
	if (!Config::Load()) {
		panic_msgbox(UiMessage_GamePathUnavailable);
		return;
	}
	if (Config::DebugConsole)
		create_console();

	const X64Game game = detect_current_game();
	if (game == X64Game::Unknown) {
		queue_ui_message(UiMessage_UnsupportedGame, MB_ICONERROR, true);
		show_queued_ui_messages();
		return;
	}

	printf("Detected %ls\n", game_name(game));
	Limiter::Initialize(game_replay_callback(game));
	X64GamePatchFunction patch = game_patch_function(game);
	if (patch == nullptr) {
		queue_ui_message(UiMessage_MissingPatchFunction, MB_ICONERROR, true);
		show_queued_ui_messages();
		return;
	}
	patch();
	show_queued_ui_messages();
}
}

BOOL APIENTRY DllMain(HMODULE, DWORD reason, LPVOID) {
	switch (reason) {
	case DLL_PROCESS_ATTACH:
		patcher_main();
		break;
	case DLL_PROCESS_DETACH:
		x64_overlay_destroy();
		break;
	}
	return TRUE;
}
