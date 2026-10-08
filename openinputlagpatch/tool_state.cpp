// See tool_state.h
//
// The value being looked for is found from the outside, without knowing anything about the tool's
// layout: it's the one value in the tool's own writable data that reads as 2 exactly where the
// tool's frame is ready to be drawn and as 0 right after the tool drew it, and that the tool's own
// draw clears when it's set by hand. Plenty of values in a module are 2 or 0 at some point, so all
// three of those have to hold - and the last one can't be fooled: only the real value is cleared by
// the tool's draw.

#include <Windows.h>
#include <TlHelp32.h>
#include <stdio.h>
#include <vector>
#include "tool_state.h"

namespace {
	// The value the tool's frame state is kept in, once it has been found
	volatile DWORD* frame_value = nullptr;

	// The search (see the top of this file)
	bool searching = false;
	bool search_over = false;
	unsigned int samples = 0;
	unsigned char* tool_draw_address = nullptr;
	std::vector<DWORD*> candidates;

	// The tool draws its overlay through a hook it installed on a plain return. Nothing hooked
	// there means there's no tool to follow.
	bool tool_present(unsigned char* hook_address) {
		return hook_address != nullptr && *hook_address != 0xC3;
	}

	bool contains_ci(const wchar_t* haystack, const wchar_t* needle) {
		for (size_t start = 0; haystack[start] != L'\0'; start++) {
			size_t i = 0;
			for (; needle[i] != L'\0'; i++) {
				wchar_t a = haystack[start + i];
				wchar_t b = needle[i];
				if (a == L'\0')
					return false;
				if (a >= L'A' && a <= L'Z')
					a = (wchar_t)(a - L'A' + L'a');
				if (b >= L'A' && b <= L'Z')
					b = (wchar_t)(b - L'A' + L'a');
				if (a != b)
					break;
			}
			if (needle[i] == L'\0')
				return true;
		}
		return false;
	}

	// The tool's module. Its name is the tool's launcher executable (that's what gets loaded into
	// the game), so this looks for the name both are built under.
	HMODULE find_tool_module() {
		HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, GetCurrentProcessId());
		if (snapshot == INVALID_HANDLE_VALUE)
			return nullptr;

		HMODULE result = nullptr;
		MODULEENTRY32W entry = {};
		entry.dwSize = sizeof(entry);
		if (Module32FirstW(snapshot, &entry)) {
			do {
				if (contains_ci(entry.szModule, L"thprac")) {
					result = entry.hModule;
					break;
				}
			} while (Module32NextW(snapshot, &entry));
		}
		CloseHandle(snapshot);
		return result;
	}

	// The writable part of the tool's image, i.e. where its globals live. The rest of the image is
	// code and read-only data, which a value like this can't be in.
	bool tool_data_regions(HMODULE module, std::vector<std::pair<unsigned char*, size_t>>& regions) {
		auto dos = (IMAGE_DOS_HEADER*)module;
		if (dos->e_magic != IMAGE_DOS_SIGNATURE)
			return false;
		auto nt = (IMAGE_NT_HEADERS*)((unsigned char*)module + dos->e_lfanew);
		if (nt->Signature != IMAGE_NT_SIGNATURE)
			return false;

		auto section = IMAGE_FIRST_SECTION(nt);
		for (unsigned i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++section) {
			if (!(section->Characteristics & IMAGE_SCN_MEM_WRITE))
				continue;
			if (section->Misc.VirtualSize == 0)
				continue;
			regions.push_back({
				(unsigned char*)module + section->VirtualAddress,
				(size_t)section->Misc.VirtualSize,
			});
		}
		return !regions.empty();
	}

	// Keeps only the values that still look like the tool's frame state
	void filter_candidates(DWORD wanted) {
		size_t kept = 0;
		for (size_t i = 0; i < candidates.size(); ++i) {
			if (*candidates[i] == wanted)
				candidates[kept++] = candidates[i];
		}
		candidates.resize(kept);
	}

	// The tool's draw clears the value when it's set, which is what tells it apart from everything
	// else that reads as 2 and 0 around the same points of the frame.
	bool find_frame_value() {
		for (size_t i = 0; i < candidates.size(); ++i) {
			*candidates[i] = 2;
			((void(*)())tool_draw_address)();

			if (*candidates[i] == 0) {
				frame_value = candidates[i];
				printf("Frame interpolation: the tool's frame state was found, its overlay is drawn for every presentation\n");
				return true;
			}

			// Not it: put back what it was before the test
			*candidates[i] = 0;
		}
		return false;
	}

	// How much of the tool's data can read as a candidate before the search gives up on it (the
	// test below runs the tool's draw for each one, and that has to stay short)
	const size_t candidates_max = 20000;

	// How many frames of looking at both points of the tool's frame are taken before the values
	// left over are tested
	const unsigned int samples_needed = 3;
}

void ToolState::WatchFrameReady(unsigned char* frame_address, unsigned char* draw_address) {
	if (search_over && frame_value == nullptr)
		return;
	if (!tool_present(frame_address))
		return;

	if (!searching) {
		// Start looking: everything in the tool's data that reads as 2 while its frame is ready to
		// be drawn is a candidate
		searching = true;
		tool_draw_address = draw_address;

		HMODULE module = find_tool_module();
		std::vector<std::pair<unsigned char*, size_t>> regions;
		if (module == nullptr || !tool_data_regions(module, regions)) {
			search_over = true;
			printf("Frame interpolation: couldn't find the tool's module, its overlay stays out of the extra presentations\n");
			return;
		}

		size_t skipped = 0;
		for (const auto& region : regions) {
			for (size_t offset = 0; offset + sizeof(DWORD) <= region.second; offset += sizeof(DWORD)) {
				auto value = (DWORD*)(region.first + offset);
				if (*value != 2)
					continue;
				if (candidates.size() < candidates_max)
					candidates.push_back(value);
				else
					++skipped;
			}
		}
		if (skipped > 0)
			printf("Frame interpolation: %u of the values that look like the tool's frame state are left out\n", (unsigned int)skipped);
		return;
	}

	// Later frames: the value has to look like that every time
	filter_candidates(2);
}

void ToolState::WatchFrameDrawn() {
	if (!searching || frame_value != nullptr)
		return;

	filter_candidates(0);
	if (candidates.empty()) {
		search_over = true;
		printf("Frame interpolation: no tool frame state in the tool's data, its overlay stays out of the extra presentations\n");
		return;
	}

	// A few frames of that leave a handful of values, and the test that follows settles which of
	// them it is
	if (++samples < samples_needed)
		return;

	search_over = true;
	if (!find_frame_value())
		printf("Frame interpolation: the tool's frame state couldn't be identified, its overlay stays out of the extra presentations\n");
}

void ToolState::ReadyToDraw() {
	if (frame_value != nullptr)
		*frame_value = 2;
}
