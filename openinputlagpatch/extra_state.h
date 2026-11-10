#pragma once

#include <stddef.h>
#include <stdlib.h>
#include <string.h>

// A value an extra presentation has to put back before it draws, and put back the way the game's own
// pass left it afterwards.
//
// Several of the games set a screen shake (or an equivalent render offset) in their calculation
// chain, read it while drawing and clear it inside their own draw pass. A game that draws once per
// frame is fine with that, but an extra presentation that repeats the draw starts from the cleared
// value and draws the frame without the offset - one shaken picture and two still ones per frame,
// which reads as a fraction of the shake. The value is therefore taken where the game's own pass
// starts, written back before each extra pass, and the value the game's own pass left is put back
// afterwards.
//
// Every one of these is the same three steps, so they live here rather than being spelled out at
// each site: an ExtraFloatState is an address plus how many floats from it belong to the offset.
// Keep the state next to the game's other scene state and use extra_float_state_save in the render
// hook, then begin/end around the object walk of an extra presentation.
struct ExtraFloatState {
    float* address;
    int count;        // floats from address, 1 to 4
    float saved[4];   // what the game's own pass started with
    float before[4];  // what was there when the extra pass began
};

inline void extra_float_state_save(ExtraFloatState* state) {
    for (int i = 0; i < state->count; ++i)
        state->saved[i] = state->address[i];
}

inline void extra_float_state_begin(ExtraFloatState* state) {
    for (int i = 0; i < state->count; ++i) {
        state->before[i] = state->address[i];
        state->address[i] = state->saved[i];
    }
}

inline void extra_float_state_end(ExtraFloatState* state) {
    for (int i = 0; i < state->count; ++i)
        state->address[i] = state->before[i];
}

// Some render callbacks advance a stage object's transition state. Repeating that walk for an extra
// presentation must not advance it again, but copying the whole object is unsafe: the render walk
// can replace resource pointers and task handles held by the same object. These helpers copy only
// the ranges the callbacks change, and skip the restore if the stage slot now holds another object.
struct ExtraByteRange {
    size_t offset;
    size_t size;
};

struct ExtraRangeState {
    void** slot;
    const ExtraByteRange* ranges;
    size_t range_count;
    size_t object_size;
    unsigned char* saved;
    void* saved_object;
    bool valid;
};

inline bool extra_ranges_fit(const ExtraRangeState* state) {
    for (size_t i = 0; i < state->range_count; ++i) {
        if (state->ranges[i].offset > state->object_size ||
            state->ranges[i].size > state->object_size - state->ranges[i].offset)
            return false;
    }
    return true;
}

inline size_t extra_range_saved_size(const ExtraRangeState* state) {
    size_t size = 0;
    for (size_t i = 0; i < state->range_count; ++i)
        size += state->ranges[i].size;
    return size;
}

inline void extra_range_state_save(ExtraRangeState* state) {
    state->valid = false;
    void* object = *state->slot;
    if (object == nullptr || !extra_ranges_fit(state))
        return;

    if (state->saved == nullptr)
        state->saved = (unsigned char*)malloc(extra_range_saved_size(state));
    if (state->saved == nullptr)
        return;

    size_t saved_offset = 0;
    for (size_t i = 0; i < state->range_count; ++i) {
        memcpy(state->saved + saved_offset, (unsigned char*)object + state->ranges[i].offset,
            state->ranges[i].size);
        saved_offset += state->ranges[i].size;
    }
    state->saved_object = object;
    state->valid = true;
}

inline void extra_range_state_restore(ExtraRangeState* state) {
    void* object = *state->slot;
    if (!state->valid || object == nullptr || object != state->saved_object ||
        !extra_ranges_fit(state)) {
        state->valid = false;
        return;
    }

    size_t saved_offset = 0;
    for (size_t i = 0; i < state->range_count; ++i) {
        memcpy((unsigned char*)object + state->ranges[i].offset, state->saved + saved_offset,
            state->ranges[i].size);
        saved_offset += state->ranges[i].size;
    }
    state->valid = false;
}
