// Game detection

#include <Windows.h>
#include <vector>
#include "sha256.h"
#include "config.h"
#include "common.h"
#include "games.h"
#include "games_def.h"
#include "games_name.h"

// Try to detect the current game
static TouhouGame detect_game_uncached() {
	if (Config::GameOverride != TouhouGame::Unknown) {
		printf("Game is being overridden to %s, things are probably about to break!\n", game_to_string[(size_t)Config::GameOverride]);
		return Config::GameOverride;
	}

	// Get the path to the game's executable
	wchar_t game_path[1024] = {};
	if (!GetModuleFileName(NULL, game_path, 1024)) {
		queue_ui_message(UiMessage_GamePathUnavailable, MB_ICONERROR, true);
		return TouhouGame::Unknown;
	}

	GameNameMatch filename_match = match_game_from_executable_name(game_path);
	TouhouGame filename_game = filename_match.game;

	// Hash it first. A recognized hash is authoritative and needs no warning.
	char hash_str[SHA256_BLOCK_SIZE * 2 + 1] = {};
	if (!sha256_file(game_path, hash_str)) {
		if (filename_game != TouhouGame::Unknown) {
			printf("Could not hash the game; executable name suggests %s\n",
				game_to_string[(size_t)filename_game]);
			queue_ui_message(UiMessage_NonOriginalGame, MB_OK | MB_ICONWARNING, false,
				filename_match.display_name);
			return filename_game;
		}
		queue_ui_message(UiMessage_GameHashFailed, MB_ICONERROR, true);
		return TouhouGame::Unknown;
	}
	printf("Game hash: %s\n", hash_str);

	// Check if the game matches any known hashes
	for (size_t i = 0; i < game_hashes.size(); i++) {
		for (size_t j = 0; j < game_hashes[i].size(); j++) {
			if (!strcmp(hash_str, game_hashes[i][j])) {
				printf("Detected game: %s\n", game_to_string[i]);
				return (TouhouGame)i;
			}
		}
	}

	// Filename fallback keeps modified or repacked variants usable, but their patch layout is no
	// longer backed by the hash of the exact executable.
	if (filename_game != TouhouGame::Unknown) {
		printf("Unrecognized game hash; executable name suggests %s\n",
			game_to_string[(size_t)filename_game]);
		queue_ui_message(UiMessage_NonOriginalGame, MB_OK | MB_ICONWARNING, false,
			filename_match.display_name);
		return filename_game;
	}

	queue_ui_message(UiMessage_UnsupportedGame, MB_ICONERROR, true);
	return TouhouGame::Unknown;
}

// Same as detect_game_uncached, but hashing the game's executable and showing an error if it
// can't be identified only happens once
TouhouGame detect_game() {
	static bool detected = false;
	static TouhouGame result = TouhouGame::Unknown;

	if (detected)
		return result;
	detected = true;

	result = detect_game_uncached();
	return result;
}

// Get the per-game patch function for a specified game
GamePatchFunction get_patch_function(TouhouGame game) {
	return game_to_patch_function[(size_t)game];
}

// Get the replay callback for a specified game
// If replay detection is unimplemented, this will return null
ReplayCallback get_replay_callback(TouhouGame game) {
	return game_to_replay_callback[(size_t)game];
}
