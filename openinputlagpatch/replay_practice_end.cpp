#include <Windows.h>
#include <stdio.h>
#include <string.h>
#include "patch_util.h"
#include "replay_practice_end.h"

static const ReplayPracticeGuard* active_guard = nullptr;
static int last_reported_stage = -1;
static int* late_exit_scene_state = nullptr;
static int late_exit_normal_state = 0;
static uintptr_t late_exit_finish_replay = 0;
static uintptr_t late_exit_normal_resume = 0;
static uintptr_t late_exit_missing_target = 0;
static uintptr_t pending_finish_replay = 0;
static unsigned int pending_finish_frames = 0;
static bool finish_hold_reported = false;

enum {
	ReplayPracticeScoreDisplayFrames = 120,
};

static bool fixed_header_exists(const ReplayPracticeGuard* guard, void* manager, int stage) {
	unsigned char* item = (unsigned char*)manager + guard->stage_table_offset +
		guard->stage_stride * stage;
	return *(void**)(item + guard->stage_header_offset) != nullptr;
}

static bool th20_header_exists(void* manager, int stage) {
	unsigned char* data = *(unsigned char**)((unsigned char*)manager + 0x1C);
	if (data == nullptr)
		return false;

	int count = *(int*)(data + 0xD4);
	if (count <= 0)
		return false;
	if (count > 8)
		count = 6;

	unsigned char* entry = data + 0x100;
	for (int i = 0; i < count; ++i) {
		if (*(int*)entry == stage)
			return true;
		int extra = *(int*)(entry + 0xC);
		if (extra < 0 || extra > 0x100000)
			return false;
		entry += 0x2A0 + extra;
	}
	return false;
}

bool replay_practice_next_stage_missing() {
	const ReplayPracticeGuard* guard = active_guard;
	if (guard == nullptr)
		return false;

	void* manager = *(void* const*)guard->manager_slot;
	if (manager == nullptr || *(int*)((unsigned char*)manager + guard->mode_offset) != 1)
		return false;

	int current_stage = *guard->stage_number;
	if (current_stage < 1 || current_stage >= 6)
		return false;

	int next_stage = current_stage + 1;
	bool exists = guard->layout == ReplayPracticeStageLayout::Th20HeaderList
		? th20_header_exists(manager, next_stage)
		: fixed_header_exists(guard, manager, next_stage);
	return !exists;
}

void replay_practice_report_end(int stage) {
	if (last_reported_stage == stage)
		return;
	last_reported_stage = stage;
	printf("Replay playback reached its recorded end at stage %d; "
		"showing the stage score before returning to the replay menu.\n", stage);
}

static bool __cdecl late_stage_exit_takes_replay_path() {
	bool missing = replay_practice_next_stage_missing();
	if (!missing) {
		*late_exit_scene_state = late_exit_normal_state;
		return false;
	}

	if (active_guard != nullptr)
		replay_practice_report_end(*active_guard->stage_number);
	if (pending_finish_frames == 0) {
		pending_finish_replay = late_exit_finish_replay;
		pending_finish_frames = ReplayPracticeScoreDisplayFrames;
	}
	return true;
}

void __declspec(naked) replay_practice_late_stage_exit_thunk() {
	__asm {
		pushfd
		pushad
		call late_stage_exit_takes_replay_path
		test al, al
		jz late_normal_path
		popad
		popfd
		push dword ptr [late_exit_missing_target]
		ret
	late_normal_path:
		popad
		popfd
		push dword ptr [late_exit_normal_resume]
		ret
	}
}

static void __cdecl finish_replay_or_hold() {
	if (pending_finish_frames != 0) {
		if (!finish_hold_reported) {
			finish_hold_reported = true;
			printf("Replay input end held until the stage score display completes.\n");
		}
		return;
	}
	if (late_exit_finish_replay != 0)
		((void (__cdecl*)())late_exit_finish_replay)();
}

void replay_practice_guard_configure(const ReplayPracticeGuard* guard) {
	active_guard = guard;
	last_reported_stage = -1;
	pending_finish_replay = 0;
	pending_finish_frames = 0;
	finish_hold_reported = false;
}

bool replay_practice_patch_late_stage_exit(const ReplayPracticeGuard* guard,
	uintptr_t finish_replay, uintptr_t address, int* scene_state, int normal_state,
	uintptr_t normal_resume, uintptr_t missing_target,
	const unsigned char* expected, size_t expected_size) {
	if (expected_size != 10 ||
		memcmp((const void*)address, expected, expected_size) != 0)
		return false;

	replay_practice_guard_configure(guard);
	late_exit_scene_state = scene_state;
	late_exit_normal_state = normal_state;
	late_exit_finish_replay = finish_replay;
	late_exit_normal_resume = normal_resume;
	late_exit_missing_target = missing_target;

	patch_jmp((void*)address, (void*)replay_practice_late_stage_exit_thunk);
	unsigned char nops[5] = { 0x90, 0x90, 0x90, 0x90, 0x90 };
	patch_bytes((void*)(address + 5), nops, sizeof(nops));
	return true;
}

bool replay_practice_patch_finish_call(uintptr_t call_site, uintptr_t finish_replay) {
	const unsigned char* bytes = (const unsigned char*)call_site;
	if (bytes[0] != 0xE8)
		return false;

	int32_t displacement = 0;
	memcpy(&displacement, bytes + 1, sizeof(displacement));
	uintptr_t target = call_site + 5 + displacement;
	if (target != finish_replay)
		return false;

	patch_call((void*)call_site, (void*)finish_replay_or_hold);
	return true;
}

void replay_practice_present_tick() {
	if (pending_finish_frames == 0)
		return;
	if (--pending_finish_frames != 0)
		return;

	uintptr_t finish_replay = pending_finish_replay;
	pending_finish_replay = 0;
	if (finish_replay != 0) {
		printf("Replay practice score display finished; returning to the replay menu.\n");
		((void (__cdecl*)())finish_replay)();
	}
}

bool replay_practice_patch_function(uintptr_t address, void* replacement,
	const unsigned char* expected, size_t expected_size) {
	if (expected_size == 0 || expected_size > 8 ||
		memcmp((const void*)address, expected, expected_size) != 0)
		return false;
	patch_jmp((void*)address, replacement);
	if (expected_size > 5) {
		unsigned char nops[3] = { 0x90, 0x90, 0x90 };
		patch_bytes((void*)(address + 5), nops, expected_size - 5);
	}
	return true;
}
