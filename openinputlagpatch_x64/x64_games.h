#pragma once

#include <Windows.h>
#include "fps_target.h"

enum class X64Game {
	Unknown,
	Th6ncOld,
};

typedef void (*X64GamePatchFunction)();

X64Game detect_game_file(const wchar_t* path);
X64Game detect_current_game();
const wchar_t* game_name(X64Game game);
const wchar_t* game_steam_appid(X64Game game);
#ifndef OILP_LOADER
X64GamePatchFunction game_patch_function(X64Game game);
ReplayCallback game_replay_callback(X64Game game);
#endif
