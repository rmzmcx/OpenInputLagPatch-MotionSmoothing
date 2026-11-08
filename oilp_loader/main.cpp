#include <Windows.h>
#include <winternl.h>
#include <stdio.h>
#include <string.h>
#include <string>
#include <vector>

#include "sha256.h"
#include "games.h"
#include "games_def.h"
#include "games_name.h"
#include "common.h"

namespace {
enum class ConsoleLanguage {
	English,
	Chinese,
	Japanese,
};

ConsoleLanguage console_language() {
	switch (PRIMARYLANGID(GetUserDefaultUILanguage())) {
	case LANG_CHINESE:
		return ConsoleLanguage::Chinese;
	case LANG_JAPANESE:
		return ConsoleLanguage::Japanese;
	default:
		return ConsoleLanguage::English;
	}
}

const wchar_t* select_game_prompt() {
	// CJK prompts use full-width parentheses and colons; English uses half-width punctuation.
	switch (console_language()) {
	case ConsoleLanguage::Chinese:
		return L"选择你的游戏（输入左边对应的序号）：";
	case ConsoleLanguage::Japanese:
		return L"ゲームを選択してください（左側の番号を入力）：";
	default:
		return L"Select your game (enter the number on the left): ";
	}
}

const wchar_t* no_games_message() {
	switch (console_language()) {
	case ConsoleLanguage::Chinese:
		return L"未找到受支持的游戏。\n";
	case ConsoleLanguage::Japanese:
		return L"対応しているゲームが見つかりません。\n";
	default:
		return L"No supported games were found.\n";
	}
}

const wchar_t* entrypoint_fallback_message() {
	switch (console_language()) {
	case ConsoleLanguage::Chinese:
		return L"未能让游戏运行到程序入口点；将改为在尚未初始化的进程中注入。\n";
	case ConsoleLanguage::Japanese:
		return L"ゲームをエントリーポイントまで実行できませんでした。未初期化のプロセスへ直接注入します。\n";
	default:
		return L"Couldn't run the game to its entrypoint; injecting into the uninitialized process instead.\n";
	}
}

void write_console(const wchar_t* text) {
	const int length = (int)wcslen(text);
	if (length == 0)
		return;

	const HANDLE output = GetStdHandle(STD_OUTPUT_HANDLE);
	DWORD mode = 0;
	if (output != INVALID_HANDLE_VALUE && output != nullptr &&
		GetConsoleMode(output, &mode)) {
		DWORD written = 0;
		if (WriteConsoleW(output, text, (DWORD)length, &written, nullptr))
			return;
	}

	const int size = WideCharToMultiByte(CP_UTF8, 0, text, length, nullptr, 0, nullptr, nullptr);
	if (size <= 0)
		return;
	std::string utf8((size_t)size, '\0');
	if (WideCharToMultiByte(CP_UTF8, 0, text, length, &utf8[0], size, nullptr, nullptr) != size)
		return;

	fflush(stdout);
	DWORD written = 0;
	WriteFile(output, utf8.data(), (DWORD)utf8.size(), &written, nullptr);
}

std::wstring widen_ascii(const char* text) {
	return std::wstring(text, text + strlen(text));
}
}

TouhouGame detect_game(wchar_t* game_path) {
	// Hash the game
	char hash_str[SHA256_BLOCK_SIZE * 2 + 1] = {};
	if (!sha256_file(game_path, hash_str)) {
		return TouhouGame::Unknown;
	}

	// Check if the game matches any known hashes
	for (size_t i = 0; i < game_hashes.size(); i++) {
		for (size_t j = 0; j < game_hashes[i].size(); j++) {
			if (!strcmp(hash_str, game_hashes[i][j])) {
				return (TouhouGame)i;
			}
		}
	}

	// Modified and repacked executables no longer match a known hash. Their executable name can
	// still identify the game layout; the injected DLL shows the non-original-version warning.
	return game_from_executable_name(game_path);
}

// Steam App IDs of games whose Steam release is protected by SteamStub
const wchar_t* game_to_steam_appid(TouhouGame game) {
	switch (game) {
	case TouhouGame::Th18: return L"1566410";
	case TouhouGame::Th19:
	case TouhouGame::Th19v100a: return L"2400340";
	case TouhouGame::Th20: return L"3671710";
	default: return nullptr;
	}
}

// Without SteamAppId set, SteamStub relaunches the game through Steam, which loses the injected DLL
void set_steam_appid(TouhouGame game) {
	if (GetEnvironmentVariableW(L"SteamAppId", NULL, 0))
		return;
	auto appid = game_to_steam_appid(game);
	if (!appid)
		return;
	SetEnvironmentVariableW(L"SteamAppId", appid);
	SetEnvironmentVariableW(L"SteamGameId", appid);
}

// Lets the suspended game run until its entrypoint, where it's held in an infinite loop and suspended again
// Injecting into a process that hasn't been initialized yet makes the injected thread the one that loads all of the game's DLLs,
// and some DLLs that get loaded alongside the game hang when that happens (Ju.N.Owen's d3d9.dll on th19 for example)
// On Steam releases this is SteamStub's entrypoint, so the game's own code is still encrypted at this point
bool run_to_entrypoint(PROCESS_INFORMATION& pi, DWORD& entry, WORD& orig_bytes) {
	auto query_process = (decltype(&NtQueryInformationProcess))GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtQueryInformationProcess");
	PROCESS_BASIC_INFORMATION pbi = {};
	if (!query_process || query_process(pi.hProcess, ProcessBasicInformation, &pbi, sizeof(pbi), NULL) != 0)
		return false;

	// PEB->ImageBaseAddress
	DWORD image_base = 0;
	IMAGE_DOS_HEADER dos_header = {};
	IMAGE_NT_HEADERS32 nt_header = {};
	if (!ReadProcessMemory(pi.hProcess, (char*)pbi.PebBaseAddress + 8, &image_base, sizeof(image_base), NULL) ||
		!ReadProcessMemory(pi.hProcess, (LPCVOID)image_base, &dos_header, sizeof(dos_header), NULL) ||
		!ReadProcessMemory(pi.hProcess, (LPCVOID)(image_base + dos_header.e_lfanew), &nt_header, sizeof(nt_header), NULL))
		return false;
	entry = image_base + nt_header.OptionalHeader.AddressOfEntryPoint;
	if (!ReadProcessMemory(pi.hProcess, (LPCVOID)entry, &orig_bytes, sizeof(orig_bytes), NULL))
		return false;

	// jmp $
	WORD loop = 0xFEEB;
	DWORD prot;
	if (!VirtualProtectEx(pi.hProcess, (LPVOID)entry, sizeof(loop), PAGE_EXECUTE_READWRITE, &prot))
		return false;
	WriteProcessMemory(pi.hProcess, (LPVOID)entry, &loop, sizeof(loop), NULL);
	FlushInstructionCache(pi.hProcess, (LPCVOID)entry, sizeof(loop));

	// Give up after 10 seconds, leaving the game suspended wherever it is
	ResumeThread(pi.hThread);
	for (int i = 0; ; i++) {
		Sleep(10);
		SuspendThread(pi.hThread);
		CONTEXT ctx = {};
		ctx.ContextFlags = CONTEXT_CONTROL;
		if (GetThreadContext(pi.hThread, &ctx) && ctx.Eip == entry)
			break;
		if (WaitForSingleObject(pi.hProcess, 0) == WAIT_OBJECT_0)
			return false;
		if (i >= 1000)
			break;
		ResumeThread(pi.hThread);
	}

	// Restore the original code whether the game made it to its entrypoint or not
	WriteProcessMemory(pi.hProcess, (LPVOID)entry, &orig_bytes, sizeof(orig_bytes), NULL);
	FlushInstructionCache(pi.hProcess, (LPCVOID)entry, sizeof(orig_bytes));
	VirtualProtectEx(pi.hProcess, (LPVOID)entry, sizeof(loop), prot, &prot);
	return true;
}

int run_game_and_inject(const wchar_t* path, TouhouGame game) {
	set_steam_appid(game);
	STARTUPINFOW si = { sizeof(si) };
	PROCESS_INFORMATION pi = {};
	if (!CreateProcessW(path, NULL, NULL, NULL, FALSE, CREATE_SUSPENDED, NULL, NULL, &si, &pi))
		return -1;
	DWORD entry = 0;
	WORD orig_bytes = 0;
	if (!run_to_entrypoint(pi, entry, orig_bytes))
		write_console(entrypoint_fallback_message());
	LPVOID remoteStr = VirtualAllocEx(pi.hProcess, NULL, 44, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
	if (!remoteStr)
		return -2;
	defer(VirtualFreeEx(pi.hProcess, remoteStr, 0, MEM_RELEASE));
	DWORD byteRet;
	if (!WriteProcessMemory(pi.hProcess, remoteStr, L"openinputlagpatch.dll", 42, &byteRet)) {
		return -3;
	}
	HANDLE hRemoteThread = CreateRemoteThread(pi.hProcess, NULL, 0, (LPTHREAD_START_ROUTINE)::LoadLibraryW, remoteStr, 0, NULL);
	if (hRemoteThread == NULL) {
		return -4;
	}
	defer(CloseHandle(hRemoteThread));
	WaitForSingleObject(hRemoteThread, INFINITE);
	DWORD exitCode = 0;
	if (!GetExitCodeThread(hRemoteThread, &exitCode) || !exitCode)
		return -5;
	ResumeThread(pi.hThread);
	return 0;
}

std::string cmd_inp() {
	std::string ret;
	for (;;) {
		const int input = getchar();
		if (input == EOF || input == '\n')
			break;
		if (input != '\r')
			ret.push_back((char)input);
	}
	return ret;
}

int wmain() {
	struct _TouhouGameFind {
		TouhouGame game;
		std::wstring path;
	};
	WIN32_FIND_DATAW find;
	HANDLE hFind = FindFirstFileW(L"*.exe", &find);
	std::vector<_TouhouGameFind> games;
	if (hFind == INVALID_HANDLE_VALUE) {
		write_console(no_games_message());
		show_ui_message(UiMessage_UnsupportedGame, MB_ICONERROR);
		return 1;
	}
	do {
		TouhouGame game = detect_game(find.cFileName);
		if (game != TouhouGame::Unknown && game != TouhouGame::Custom) {
			games.push_back({ game, find.cFileName });
		}
	} while (FindNextFileW(hFind, &find));
	FindClose(hFind);
	if (games.size() == 1)
		return run_game_and_inject(games[0].path.c_str(), games[0].game);
	else if (games.size() == 0) {
		write_console(no_games_message());
		show_ui_message(UiMessage_UnsupportedGame, MB_ICONERROR);
		return 1;
	}
	for (size_t i = 0; i < games.size(); i++) {
		const std::wstring line =
			L"[" + std::to_wstring(i) + L"]: " +
			widen_ascii(game_to_string[(size_t)games[i].game]) +
			L" (" + games[i].path + L")\n";
		write_console(line.c_str());
	}
	
	size_t sel = 0;
	do {
		write_console(select_game_prompt());
		try {
			sel = std::stoul(cmd_inp());
		} catch (...) {
			sel = games.size();
		}
	} while (sel >= games.size());
	return run_game_and_inject(games[sel].path.c_str(), games[sel].game);
}
