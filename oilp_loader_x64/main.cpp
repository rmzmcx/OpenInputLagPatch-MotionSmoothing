#include <Windows.h>
#include <winternl.h>
#include <conio.h>
#include <stdio.h>
#include <tlhelp32.h>
#include <vector>
#include <string>
#include "x64_games.h"

namespace {
struct FoundGame {
	X64Game game;
	std::wstring path;
};

bool run_to_entrypoint(PROCESS_INFORMATION& pi, uintptr_t& entry, WORD& original_bytes) {
	typedef NTSTATUS(NTAPI* NtQueryInformationProcess_t)(
		HANDLE, PROCESSINFOCLASS, PVOID, ULONG, PULONG);
	auto query_process = (NtQueryInformationProcess_t)GetProcAddress(
		GetModuleHandleW(L"ntdll.dll"), "NtQueryInformationProcess");
	PROCESS_BASIC_INFORMATION info = {};
	if (query_process == nullptr ||
		query_process(pi.hProcess, ProcessBasicInformation, &info, sizeof(info), nullptr) != 0) {
		return false;
	}

	uintptr_t image_base = 0;
	IMAGE_DOS_HEADER dos = {};
	IMAGE_NT_HEADERS64 nt = {};
	if (!ReadProcessMemory(pi.hProcess, (BYTE*)info.PebBaseAddress + 0x10,
			&image_base, sizeof(image_base), nullptr) ||
		!ReadProcessMemory(pi.hProcess, (LPCVOID)image_base, &dos, sizeof(dos), nullptr) ||
		!ReadProcessMemory(pi.hProcess, (LPCVOID)(image_base + dos.e_lfanew),
			&nt, sizeof(nt), nullptr)) {
		return false;
	}
	if (nt.OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC)
		return false;

	entry = image_base + nt.OptionalHeader.AddressOfEntryPoint;
	if (!ReadProcessMemory(pi.hProcess, (LPCVOID)entry, &original_bytes,
			sizeof(original_bytes), nullptr)) {
		return false;
	}

	const WORD loop = 0xFEEB;
	DWORD protection = 0;
	if (!VirtualProtectEx(pi.hProcess, (LPVOID)entry, sizeof(loop),
			PAGE_EXECUTE_READWRITE, &protection)) {
		return false;
	}
	WriteProcessMemory(pi.hProcess, (LPVOID)entry, &loop, sizeof(loop), nullptr);
	FlushInstructionCache(pi.hProcess, (LPCVOID)entry, sizeof(loop));

	ResumeThread(pi.hThread);
	for (int i = 0; i < 1000; ++i) {
		Sleep(10);
		SuspendThread(pi.hThread);
		CONTEXT context = {};
		context.ContextFlags = CONTEXT_CONTROL;
		if (GetThreadContext(pi.hThread, &context) && context.Rip == entry)
			break;
		if (WaitForSingleObject(pi.hProcess, 0) == WAIT_OBJECT_0)
			break;
		ResumeThread(pi.hThread);
	}

	WriteProcessMemory(pi.hProcess, (LPVOID)entry, &original_bytes,
		sizeof(original_bytes), nullptr);
	FlushInstructionCache(pi.hProcess, (LPCVOID)entry, sizeof(original_bytes));
	DWORD ignored = 0;
	VirtualProtectEx(pi.hProcess, (LPVOID)entry, sizeof(loop), protection, &ignored);
	return true;
}

int inject_library(HANDLE process, const wchar_t* dll_name) {
	const SIZE_T byte_count = (wcslen(dll_name) + 1) * sizeof(wchar_t);
	LPVOID remote_string = VirtualAllocEx(process, nullptr, byte_count,
		MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
	if (remote_string == nullptr)
		return -2;
	if (!WriteProcessMemory(process, remote_string, dll_name, byte_count, nullptr)) {
		VirtualFreeEx(process, remote_string, 0, MEM_RELEASE);
		return -3;
	}

	auto load_library = (LPTHREAD_START_ROUTINE)
		GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "LoadLibraryW");
	HANDLE thread = CreateRemoteThread(process, nullptr, 0, load_library,
		remote_string, 0, nullptr);
	if (thread == nullptr) {
		VirtualFreeEx(process, remote_string, 0, MEM_RELEASE);
		return -4;
	}

	WaitForSingleObject(thread, INFINITE);
	DWORD module = 0;
	GetExitCodeThread(thread, &module);
	CloseHandle(thread);
	VirtualFreeEx(process, remote_string, 0, MEM_RELEASE);
	return module != 0 ? 0 : -5;
}

int run_game_and_inject(const wchar_t* path, X64Game game) {
	const wchar_t* appid = game_steam_appid(game);
	if (appid != nullptr && GetEnvironmentVariableW(L"SteamAppId", nullptr, 0) == 0) {
		SetEnvironmentVariableW(L"SteamAppId", appid);
		SetEnvironmentVariableW(L"SteamGameId", appid);
	}
	STARTUPINFOW startup = { sizeof(startup) };
	PROCESS_INFORMATION process = {};
	if (!CreateProcessW(path, nullptr, nullptr, nullptr, FALSE, CREATE_SUSPENDED,
			nullptr, nullptr, &startup, &process)) {
		return -1;
	}

	uintptr_t entry = 0;
	WORD original_bytes = 0;
	if (!run_to_entrypoint(process, entry, original_bytes))
		puts("Could not run to the game entrypoint; injecting into the suspended process.");

	const int result = inject_library(process.hProcess, L"openinputlagpatch_x64.dll");
	if (result != 0) {
		TerminateProcess(process.hProcess, 1);
		CloseHandle(process.hThread);
		CloseHandle(process.hProcess);
		return result;
	}

	ResumeThread(process.hThread);
	CloseHandle(process.hThread);
	CloseHandle(process.hProcess);
	return 0;
}

std::wstring absolute_path(const wchar_t* path) {
	wchar_t full_path[32768] = {};
	const DWORD length = GetFullPathNameW(path, _countof(full_path), full_path, nullptr);
	return length != 0 && length < _countof(full_path) ? full_path : path;
}

DWORD find_running_game(const wchar_t* path) {
	const std::wstring wanted = absolute_path(path);
	HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
	if (snapshot == INVALID_HANDLE_VALUE)
		return 0;

	PROCESSENTRY32W entry = { sizeof(entry) };
	DWORD found = 0;
	if (Process32FirstW(snapshot, &entry)) {
		do {
			HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, entry.th32ProcessID);
			if (process == nullptr)
				continue;
			wchar_t process_path[32768] = {};
			DWORD length = _countof(process_path);
			const bool matches = QueryFullProcessImageNameW(process, 0, process_path, &length) &&
				_wcsicmp(absolute_path(process_path).c_str(), wanted.c_str()) == 0;
			CloseHandle(process);
			if (matches) {
				found = entry.th32ProcessID;
				break;
			}
		} while (Process32NextW(snapshot, &entry));
	}
	FindClose(snapshot);
	return found;
}

int attach_to_game(const wchar_t* path) {
	const DWORD pid = find_running_game(path);
	if (pid == 0)
		return 1;
	HANDLE process = OpenProcess(
		PROCESS_CREATE_THREAD | PROCESS_QUERY_INFORMATION | PROCESS_VM_OPERATION |
			PROCESS_VM_WRITE | PROCESS_VM_READ, FALSE, pid);
	if (process == nullptr)
		return 2;
	const int result = inject_library(process, L"openinputlagpatch_x64.dll");
	CloseHandle(process);
	if (result == 0)
		printf("Injected into running th06nc process %lu.\n", pid);
	return result;
}

std::string read_line() {
	std::string result;
	for (;;) {
		const int character = getchar();
		if (character == '\n' || character == EOF)
			break;
		result.push_back((char)character);
	}
	return result;
}
}

int wmain(int argc, wchar_t** argv) {
	WIN32_FIND_DATAW find = {};
	HANDLE search = FindFirstFileW(L"*.exe", &find);
	std::vector<FoundGame> games;
	if (search != INVALID_HANDLE_VALUE) {
		do {
			const X64Game game = detect_game_file(find.cFileName);
			if (game != X64Game::Unknown)
				games.push_back({ game, find.cFileName });
		} while (FindNextFileW(search, &find));
		FindClose(search);
	}

	if (games.empty()) {
		puts("No supported x64 games found.");
		return 1;
	}
	bool attach = false;
	for (int i = 1; i < argc; ++i) {
		if (_wcsicmp(argv[i], L"--attach") == 0)
			attach = true;
	}

	if (games.size() == 1) {
		if (attach)
			return attach_to_game(games[0].path.c_str());
		if (find_running_game(games[0].path.c_str()) != 0)
			return attach_to_game(games[0].path.c_str());
		return run_game_and_inject(games[0].path.c_str(), games[0].game);
	}

	for (size_t i = 0; i < games.size(); ++i)
		wprintf(L"[%zu] %ls (%ls)\n", i, game_name(games[i].game), games[i].path.c_str());

	size_t selected = games.size();
	do {
		printf("Pick your game: ");
		try {
			selected = std::stoul(read_line());
		} catch (...) {
			selected = games.size();
		}
	} while (selected >= games.size());
	if (attach || find_running_game(games[selected].path.c_str()) != 0)
		return attach_to_game(games[selected].path.c_str());
	return run_game_and_inject(games[selected].path.c_str(), games[selected].game);
}
