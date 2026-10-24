#pragma once

#include <stdint.h>

void present_diag_set_window(void* window);

// Records one completed presentation at a QPC timestamp. This is called from
// overlay_mark_presentation so every D3D8 and D3D9 game uses the same timing source.
void present_diag_mark_presentation(uint64_t qpc);

// Records how long the unhooked device Present call blocked. Paths that cannot observe the
// game's own D3D9 Present leave the duration unknown.
void present_diag_note_present_duration(uint64_t start_qpc, uint64_t end_qpc);

