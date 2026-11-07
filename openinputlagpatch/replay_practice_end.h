#pragma once

#include <stddef.h>
#include <stdint.h>

enum class ReplayPracticeStageLayout {
	FixedHeaderTable,
	Th20HeaderList,
};

struct ReplayPracticeGuard {
	const void* const* manager_slot;
	size_t mode_offset;
	int* stage_number;
	size_t stage_table_offset;
	size_t stage_stride;
	size_t stage_header_offset;
	ReplayPracticeStageLayout layout;
};

void replay_practice_guard_configure(const ReplayPracticeGuard* guard);
bool replay_practice_next_stage_missing();
void replay_practice_report_end(int stage);
bool replay_practice_patch_late_stage_exit(const ReplayPracticeGuard* guard,
	uintptr_t finish_replay, uintptr_t address, int* scene_state, int normal_state,
	uintptr_t normal_resume, uintptr_t missing_target,
	const unsigned char* expected, size_t expected_size);
bool replay_practice_patch_finish_call(uintptr_t call_site, uintptr_t finish_replay);
void replay_practice_present_tick();

void replay_practice_late_stage_exit_thunk();

bool replay_practice_patch_function(uintptr_t address, void* replacement,
	const unsigned char* expected, size_t expected_size);
