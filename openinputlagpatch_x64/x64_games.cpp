#include <Windows.h>
#include <string.h>
#include "common.h"
#include "sha256.h"
#ifndef OILP_LOADER
#include "games\touhou6nc.h"
#endif
#include "x64_games.h"

namespace {
struct X64GameVersion {
	X64Game game;
	const char* hash;
	const wchar_t* name;
};

const X64GameVersion versions[] = {
	{
		X64Game::Th6ncOld,
		"07850c8c6e469c0e82c13423e6d0d096a88d693455bdacacbb44c0aa3bcce473",
		L"Touhou 6 NC (old analysis build)",
	},
};
}

X64Game detect_game_file(const wchar_t* path) {
	char hash[SHA256_BLOCK_SIZE * 2 + 1] = {};
	if (!sha256_file((wchar_t*)path, hash))
		return X64Game::Unknown;
	for (const X64GameVersion& version : versions) {
		if (strcmp(hash, version.hash) == 0)
			return version.game;
	}
	return X64Game::Unknown;
}

X64Game detect_current_game() {
	wchar_t path[1024] = {};
	if (!GetModuleFileNameW(nullptr, path, _countof(path)))
		return X64Game::Unknown;
	return detect_game_file(path);
}

const wchar_t* game_name(X64Game game) {
	for (const X64GameVersion& version : versions) {
		if (version.game == game)
			return version.name;
	}
	return L"Unknown x64 game";
}

const wchar_t* game_steam_appid(X64Game game) {
	switch (game) {
	case X64Game::Th6ncOld:
		return L"4659620";
	default:
		return nullptr;
	}
}

#ifndef OILP_LOADER
X64GamePatchFunction game_patch_function(X64Game game) {
	switch (game) {
	case X64Game::Th6ncOld:
		return th06nc_install_patches;
	default:
		return nullptr;
	}
}

ReplayCallback game_replay_callback(X64Game game) {
	switch (game) {
	case X64Game::Th6ncOld:
		return th06nc_replay_callback;
	default:
		return nullptr;
	}
}
#endif
