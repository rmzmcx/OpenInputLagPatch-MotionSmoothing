#pragma once

#include <vector>
#include <string>

// Games are added as support is implemented
// Not ordered in chronological order of when they were released
enum class TouhouGame {
	Unknown = -1,
	Custom = 0,
	Th6 = 1,
	Th10 = 2,
	Th7 = 3,
	Th8 = 4,
	Th9 = 5,
	Th11 = 6,
	Th12 = 7,
	Th13 = 8,
	Th14 = 9,
	Th15 = 10,
	Th16 = 11,
	Th17 = 12,
	Th18 = 13,
	Th95 = 14,
	Th125 = 15,
	Th128 = 16,
	Th143 = 17,
	Th165 = 18,
	Th185 = 19,
	Th19 = 20,
	Th20 = 21,
	Th19v100a = 22,
	MaxValue, // Dummy value used for config validation
};

enum class FPSTarget {
	Game,
	ReplaySkip,
	ReplaySlow,
};

// Function type for per-game patch functions
typedef void(*GamePatchFunction)();

// Per-game callback to check if we're currently in a replay and if the player wants to change speeds
typedef FPSTarget(*ReplayCallback)();

// Per-game callback that writes the diagnostics file. It is called from the present hook every game
// runs through, so it works whether or not the frame interpolation is on.
typedef void(*DiagDumpFunction)();

TouhouGame detect_game();

#ifndef OILP_LOADER
GamePatchFunction get_patch_function(TouhouGame game);
ReplayCallback get_replay_callback(TouhouGame game);
DiagDumpFunction get_diag_dump_function(TouhouGame game);
#endif

void th6_diag_dump();
void th7_diag_dump();
void th8_diag_dump();
void th10_diag_dump();
void th11_diag_dump();
void th12_diag_dump();
void th125_diag_dump();
void th13_diag_dump();
void th143_diag_dump();
void th14_diag_dump();
void th15_diag_dump();
void th16_diag_dump();
void th17_diag_dump();
void th18_diag_dump();
void th185_diag_dump();
void th19_diag_dump();
void th20_diag_dump();

void th6_install_patches();
FPSTarget th6_replay_callback();
void th10_install_patches();
FPSTarget th10_replay_callback();
void th7_install_patches();
FPSTarget th7_replay_callback();
void th8_install_patches();
FPSTarget th8_replay_callback();
void th9_install_patches();
FPSTarget th9_replay_callback();
void th11_install_patches();
FPSTarget th11_replay_callback();
void th12_install_patches();
FPSTarget th12_replay_callback();
void th13_install_patches();
FPSTarget th13_replay_callback();
void th14_install_patches();
FPSTarget th14_replay_callback();
void th15_install_patches();
FPSTarget th15_replay_callback();
void th16_install_patches();
FPSTarget th16_replay_callback();
void th17_install_patches();
FPSTarget th17_replay_callback();
void th18_install_patches();
FPSTarget th18_replay_callback();
void th95_install_patches();
FPSTarget th95_replay_callback();
void th125_install_patches();
FPSTarget th125_replay_callback();
void th128_install_patches();
FPSTarget th128_replay_callback();
void th143_install_patches();
FPSTarget th143_replay_callback();
void th165_install_patches();
FPSTarget th165_replay_callback();
void th185_install_patches();
void th19_install_patches();
void th20_install_patches();
FPSTarget th20_replay_callback();
void th19_v100a_install_patches();
