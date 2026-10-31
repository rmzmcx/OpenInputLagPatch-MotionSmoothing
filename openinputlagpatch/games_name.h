#pragma once

#include <cwchar>
#include "games.h"

struct GameNameMatch {
	TouhouGame game;
	const wchar_t* display_name;
};

// Last-resort identification for unrecognized executable variants. The hash remains the
// authoritative check; this only decides which game's patch layout is likely to fit.
inline GameNameMatch match_game_from_executable_name(const wchar_t* path) {
	if (path == nullptr)
		return { TouhouGame::Unknown, nullptr };

	const wchar_t* name = path;
	for (const wchar_t* cursor = path; *cursor != L'\0'; ++cursor) {
		if (*cursor == L'\\' || *cursor == L'/')
			name = cursor + 1;
	}

	struct GameCode {
		unsigned int number;
		TouhouGame game;
		const wchar_t* display_name;
	};

	static const GameCode codes[] = {
		{ 6, TouhouGame::Th6, L"th06" },
		{ 7, TouhouGame::Th7, L"th07" },
		{ 8, TouhouGame::Th8, L"th08" },
		{ 9, TouhouGame::Th9, L"th09" },
		{ 10, TouhouGame::Th10, L"th10" },
		{ 11, TouhouGame::Th11, L"th11" },
		{ 12, TouhouGame::Th12, L"th12" },
		{ 13, TouhouGame::Th13, L"th13" },
		{ 14, TouhouGame::Th14, L"th14" },
		{ 15, TouhouGame::Th15, L"th15" },
		{ 16, TouhouGame::Th16, L"th16" },
		{ 17, TouhouGame::Th17, L"th17" },
		{ 18, TouhouGame::Th18, L"th18" },
		{ 19, TouhouGame::Th19, L"th19" },
		{ 20, TouhouGame::Th20, L"th20" },
		{ 95, TouhouGame::Th95, L"th09.5" },
		{ 125, TouhouGame::Th125, L"th12.5" },
		{ 128, TouhouGame::Th128, L"th12.8" },
		{ 143, TouhouGame::Th143, L"th14.3" },
		{ 165, TouhouGame::Th165, L"th16.5" },
		{ 185, TouhouGame::Th185, L"th18.5" },
	};

	for (const wchar_t* cursor = name; cursor[0] != L'\0' && cursor[1] != L'\0'; ++cursor) {
		const bool is_th = (cursor[0] == L't' || cursor[0] == L'T') &&
			(cursor[1] == L'h' || cursor[1] == L'H');
		if (!is_th || cursor[2] < L'0' || cursor[2] > L'9')
			continue;

		const wchar_t* digits = cursor + 2;
		unsigned int number = 0;
		unsigned int digit_count = 0;
		while (*digits >= L'0' && *digits <= L'9') {
			if (digit_count < 6)
				number = number * 10 + (unsigned int)(*digits - L'0');
			++digits;
			++digit_count;
		}

		if (digit_count <= 6) {
			for (const GameCode& code : codes) {
				if (code.number == number)
					return { code.game, code.display_name };
			}
		}

		cursor = digits - 1;
	}

	// Keep the established localized EoSD executable names working even though they do not
	// contain a thNN token.
	if (_wcsnicmp(name, L"東方紅魔郷", 5) == 0 || _wcsnicmp(name, L"东方红魔乡", 5) == 0)
		return { TouhouGame::Th6, L"th06" };

	return { TouhouGame::Unknown, nullptr };
}

inline TouhouGame game_from_executable_name(const wchar_t* path) {
	return match_game_from_executable_name(path).game;
}
