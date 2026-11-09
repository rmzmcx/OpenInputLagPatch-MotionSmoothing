#pragma once

enum class FPSTarget {
	Game,
	ReplaySkip,
	ReplaySlow,
};

// Per-game callback to check if we're currently in a replay and if the player wants to change speeds
typedef FPSTarget (*ReplayCallback)();
