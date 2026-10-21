// Touhou 9.5: Shoot the Bullet v1.02a

#include <stdio.h>
#include <string.h>
#include <Windows.h>
#include <TlHelp32.h>
#include "d3d8/d3d8.h"
#include "patch_util.h"
#include "limiter.h"
#include "d3d8_hook.h"
#include "overlay.h"
#include "tool_state.h"
#include "window_mode.h"
#include "config.h"
#include "touhou95.h"

using namespace Touhou95;

CReplayManager** CReplayManager::InstancePtr = (CReplayManager**)0x004C4E74;

// The game's draw pass uses globals rather than an argument: the window/render object is the
// object at 0x4C4670, AnmManager is the pointer at 0x4CA1B8, and the draw chain is the second
// chain in the object at 0x4BE3C8. Render calls the chain at 0x4208AE:
//
//     4208AE  mov ecx, offset unk_4BE3C8
//     4208B3  call sub_418DA0
//
// 418DA0 is a plain-ret thiscall function. Every draw callback reachable from it was checked for
// calls to the game's animation ticker, 43A600; none of them calls it. Repeating the chain therefore
// does not advance sprite scripts, game timers, input or random state.
static unsigned char* th95_game_window = (unsigned char*)0x004C4670;
static void** th95_anm_manager = (void**)0x004CA1B8;
static unsigned char* th95_chain = (unsigned char*)0x004BE3C8;

static IDirect3DDevice8* th95_device() {
    return *(IDirect3DDevice8**)0x004C4678;
}

static auto th95_reset_vertex_buffer = (void(__fastcall*)(void*))0x0043F2B0;
static auto th95_flush_vertex_buffer = (void(__fastcall*)(void*))0x0043F2F0;
static auto th95_disable_fog = (int(__fastcall*)(void*))0x00425520;
static auto th95_run_draw_chain = (int(__fastcall*)(void*))0x00418DA0;
static auto th95_game_present = (int(__cdecl*)())0x00420920;

// ---------------------------------------------------------------------------
// Frame skeleton and the presentation boundary
// ---------------------------------------------------------------------------
//
// Render (420770) runs its calculation chain at 420811 and its draw condition at 420861. The draw
// block is 420876..4208EA:
//
//     420884  BeginScene
//     420890  ResetVertexBuffer
//     420895  fog state = 255
//     4208A4  DisableFog
//     4208AE  RunDrawChain
//     4208B9  FlushVertexBuffer
//     4208D0  SetTexture(0, NULL)
//     4208E4  EndScene
//
// 420920 is the game's whole presentation tail, not only the device call:
//
//     42093F  device->Present
//     42097C  421D00: consume the screenshot-texture request
//     420986  check the game's snapshot key in [0x4BE21E]
//     4209xx  write snapshot/th%03d.bmp
//
// 421D00 has exactly one caller, 420920. Its copy helpers 444620 and 444760 have exactly one caller,
// 421D00. The original 420920 therefore completes the shutter and photo buffer work before any
// extra drawing. The extra path never calls 420920, 421D00 or either copy helper.
//
// A frame is drawn from the state left by the calculation chain. The scene snapshot below is taken
// just before the game's own RunDrawChain and put back before each repeat. Object positions, input,
// timers and random state are not written by stage 0 at all.
static bool th95_drew_frame;
static bool th95_in_extra_presentation;
static unsigned int th95_extra_game_present_calls;
static unsigned int th95_photo_request_changes;
static unsigned int th95_thprac_redraws;
static unsigned int th95_thprac_module_misses;
static unsigned int th95_thprac_get_draw_signature_mismatches;
static unsigned int th95_thprac_dx8_signature_mismatches;
static unsigned int th95_thprac_dx9_signature_mismatches;
static unsigned int th95_thprac_device_null;
static unsigned int th95_thprac_draw_data_null;
static unsigned int th95_thprac_guireinit_count;
static unsigned int th95_thprac_guireinit_signature_mismatches;
static bool th95_thprac_guireinit_pending;
static unsigned int th95_device_reset_count;
static void* th95_last_device_reset_caller;
static int th95_startup_chain_result;
static int th95_startup_chain_first_result;
static unsigned int th95_startup_chain_calls;
static unsigned int th95_startup_flags_before;
static unsigned int th95_startup_flags_after;
static unsigned int th95_startup_reset_flag_before;
static unsigned int th95_startup_reset_flag_after;
static int th95_last_window_update_result;
static unsigned int th95_window_update_nonzero;
static unsigned int th95_window_update_calls;

static bool th95_interpolation_install_attempted;

struct Th95SceneState {
    struct BackgroundVmDrawState {
        float position[3];
        float scale[2];
    };

    D3DVIEWPORT8 device_viewport;
    D3DMATRIX device_view;
    D3DMATRIX device_projection;
    D3DMATRIX device_world[4];
    D3DMATRIX device_texture[8];
    bool has_transforms;
    bool has_world[4];
    bool has_texture[8];

    // 423440, the first calc callback, resets AnmManager's currentTexture at +0x1760, the draw
    // cache bytes at +0x1764..+0x1768, the colour fields below, and the camera translation used by
    // 43ECD0 at +0x20/+0x24. The vertex-buffer cache is at +0x17C4.
    //
    // The screenshot request ids at +0x08/+0x0C are deliberately not part of this snapshot.
    // 421D00 consumes those two ids immediately after the game's Present and writes -1 to both.
    // Putting the pre-Present values back for an extra would make the next game Present copy the
    // live screen into the same photo slots again. The draw itself never changes these ids.
    unsigned char anm_head_before_requests[0x08];
    unsigned char anm_head_after_requests[0x18];
    unsigned char anm_draw_cache[0x10];
    unsigned int anm_vertex_buffer_cache;

    // BackGroundInf is the 0x201C-byte object allocated by 4020C0 at 0x4BDD90. Do not copy the
    // whole object: its head contains the stage-data allocation and layer pointers, and its tail
    // contains Anm resources and chain nodes. Restoring those stale pointers crashes the next stage
    // load. The draw callback 402F60 writes only these fields in each of the eight VMs at +0xF8.
    unsigned char background_state[0x20];
    BackgroundVmDrawState background_vm[8];

    // 404B10 selects one of the 0xF0-byte viewport/camera layers, stores its pointer at window+0x3C4
    // and its index at +0x3C8, then calls 425910 to build the matrices and call SetViewport.
    void* view_layer;
    unsigned int view_layer_index;

    // 4235D0 advances the "Press Shot Button" HUD counter at window+0x714 once per draw.
    int shutter_prompt_frame;

    bool captured;
};

static Th95SceneState th95_scene;
static Th95SceneState th95_scene_after_game_draw;

static void th95_capture_scene_state(Th95SceneState* state) {
    IDirect3DDevice8* device = th95_device();
    void* anm = th95_anm_manager != nullptr ? *th95_anm_manager : nullptr;
    unsigned char* background = *(unsigned char**)0x004BDD90;

    state->captured = device != nullptr;
    state->has_transforms = false;
    memset(state->has_world, 0, sizeof(state->has_world));
    memset(state->has_texture, 0, sizeof(state->has_texture));
    if (device != nullptr) {
        device->GetViewport(&state->device_viewport);
        state->has_transforms =
            SUCCEEDED(device->GetTransform(D3DTS_VIEW, &state->device_view)) &&
            SUCCEEDED(device->GetTransform(D3DTS_PROJECTION, &state->device_projection));
        for (unsigned int i = 0; i < 4; ++i) {
            state->has_world[i] = SUCCEEDED(
                device->GetTransform(D3DTS_WORLDMATRIX(i), &state->device_world[i]));
        }
        for (unsigned int i = 0; i < 8; ++i) {
            state->has_texture[i] = SUCCEEDED(
                device->GetTransform((D3DTRANSFORMSTATETYPE)(D3DTS_TEXTURE0 + i),
                                     &state->device_texture[i]));
        }

    }

    if (anm != nullptr) {
        memcpy(state->anm_head_before_requests, anm,
               sizeof(state->anm_head_before_requests));
        memcpy(state->anm_head_after_requests, (unsigned char*)anm + 0x10,
               sizeof(state->anm_head_after_requests));
        memcpy(state->anm_draw_cache, (unsigned char*)anm + 0x1760,
               sizeof(state->anm_draw_cache));
        state->anm_vertex_buffer_cache =
            *(unsigned int*)((unsigned char*)anm + 0x17C4);
    }

    if (background != nullptr)
    {
        memcpy(state->background_state, background + 0x175C,
               sizeof(state->background_state));
        for (unsigned int i = 0; i < 8; ++i) {
            unsigned char* vm = background + 0xF8 + i * 0x2CC;
            memcpy(state->background_vm[i].position, vm + 0x154, sizeof(float) * 3);
            memcpy(state->background_vm[i].scale, vm + 0x30, sizeof(float) * 2);
        }
    }

    state->view_layer = *(void**)(th95_game_window + 0x3C4);
    state->view_layer_index = *(unsigned int*)(th95_game_window + 0x3C8);
    state->shutter_prompt_frame = *(int*)(th95_game_window + 0x714);
}

static void th95_restore_scene_state(const Th95SceneState* state) {
    IDirect3DDevice8* device = th95_device();
    void* anm = th95_anm_manager != nullptr ? *th95_anm_manager : nullptr;
    unsigned char* background = *(unsigned char**)0x004BDD90;

    if (state->captured && device != nullptr) {
        device->SetViewport(&state->device_viewport);
        if (state->has_transforms) {
            device->SetTransform(D3DTS_VIEW, &state->device_view);
            device->SetTransform(D3DTS_PROJECTION, &state->device_projection);
        }
        for (unsigned int i = 0; i < 4; ++i) {
            if (state->has_world[i])
                device->SetTransform(D3DTS_WORLDMATRIX(i), &state->device_world[i]);
        }
        for (unsigned int i = 0; i < 8; ++i) {
            if (state->has_texture[i]) {
                device->SetTransform((D3DTRANSFORMSTATETYPE)(D3DTS_TEXTURE0 + i),
                                     &state->device_texture[i]);
            }
        }
    }

    if (anm != nullptr) {
        memcpy(anm, state->anm_head_before_requests,
               sizeof(state->anm_head_before_requests));
        memcpy((unsigned char*)anm + 0x10, state->anm_head_after_requests,
               sizeof(state->anm_head_after_requests));
        memcpy((unsigned char*)anm + 0x1760, state->anm_draw_cache,
               sizeof(state->anm_draw_cache));
        *(unsigned int*)((unsigned char*)anm + 0x17C4) =
            state->anm_vertex_buffer_cache;
    }

    if (background != nullptr)
    {
        memcpy(background + 0x175C, state->background_state,
               sizeof(state->background_state));
        for (unsigned int i = 0; i < 8; ++i) {
            unsigned char* vm = background + 0xF8 + i * 0x2CC;
            memcpy(vm + 0x154, state->background_vm[i].position, sizeof(float) * 3);
            memcpy(vm + 0x30, state->background_vm[i].scale, sizeof(float) * 2);
        }
    }

    *(void**)(th95_game_window + 0x3C4) = state->view_layer;
    *(unsigned int*)(th95_game_window + 0x3C8) = state->view_layer_index;
    *(int*)(th95_game_window + 0x714) = state->shutter_prompt_frame;
}

// The game's own frame rate is counted from the draw chain:
//
//     423840 (draw priority 30) increments window+0x788
//     423790 (draw priority 23) calls 424720 and formats "%2.1ffps"
//
// 424720 works over +0x788, the lag numerator/denominator at +0x78C/+0x794, the value at +0x79C,
// dbl_4C4E30/dword_4C4E2C, the frame markers at 4C461C..4C4634, Frequency and game flags. PhotoInf
// records 100 - numerator/denominator*100 at 42B6F5..42B714, so a corrupted denominator changes the
// slow rate stored with a photo as well as the game's own FPS text. This state is saved around each
// extra pass and restored bit for bit.
struct Th95FpsState {
    unsigned int draw_pass_count;   // 0x4C4DF8, window+0x788
    double lag_numerator;           // 0x4C4DFC, window+0x78C
    double lag_denominator;         // 0x4C4E04, window+0x794
    float frame_rate;               // 0x4C4E0C, window+0x79C

    double fps_window_start;        // dbl_4C4E30
    unsigned int over_65_windows;   // dword_4C4E2C
    double frame_marker_base;       // dbl_4C4634
    double frame_marker_a;          // dbl_4C462C
    double frame_marker_b;          // dbl_4C461C
    double frame_marker_c;          // dbl_4C4624
    LARGE_INTEGER frequency;        // Frequency, 0x4C45FC
    unsigned int game_manager_flags;
    int text_colour;                // dword_4B1FEC
};

static void th95_fps_state_save(Th95FpsState* state) {
    unsigned char* game_manager = *(unsigned char**)0x004BDEC8;

    state->draw_pass_count = *(unsigned int*)0x004C4DF8;
    state->lag_numerator = *(double*)0x004C4DFC;
    state->lag_denominator = *(double*)0x004C4E04;
    state->frame_rate = *(float*)0x004C4E0C;

    state->fps_window_start = *(double*)0x004C4E30;
    state->over_65_windows = *(unsigned int*)0x004C4E2C;
    state->frame_marker_base = *(double*)0x004C4634;
    state->frame_marker_a = *(double*)0x004C462C;
    state->frame_marker_b = *(double*)0x004C461C;
    state->frame_marker_c = *(double*)0x004C4624;
    state->frequency = *(LARGE_INTEGER*)0x004C45FC;
    state->game_manager_flags = game_manager != nullptr ? *(unsigned int*)(game_manager + 0xFC) : 0;
    state->text_colour = *(int*)0x004B1FEC;
}

static void th95_fps_state_restore(const Th95FpsState* state) {
    unsigned char* game_manager = *(unsigned char**)0x004BDEC8;

    *(unsigned int*)0x004C4DF8 = state->draw_pass_count;
    *(double*)0x004C4DFC = state->lag_numerator;
    *(double*)0x004C4E04 = state->lag_denominator;
    *(float*)0x004C4E0C = state->frame_rate;

    *(double*)0x004C4E30 = state->fps_window_start;
    *(unsigned int*)0x004C4E2C = state->over_65_windows;
    *(double*)0x004C4634 = state->frame_marker_base;
    *(double*)0x004C462C = state->frame_marker_a;
    *(double*)0x004C461C = state->frame_marker_b;
    *(double*)0x004C4624 = state->frame_marker_c;
    *(LARGE_INTEGER*)0x004C45FC = state->frequency;
    if (game_manager != nullptr)
        *(unsigned int*)(game_manager + 0xFC) = state->game_manager_flags;
    *(int*)0x004B1FEC = state->text_colour;
}

// These two values are AnmManager's screenshot texture requests. 421D00 consumes them immediately
// after the game's Present. Drawing itself never consumes them, so a change around an extra pass
// would mean photo work had leaked into the repeat.
struct Th95PhotoRequestState {
    int first_request;       // AnmManager+0x0C
    int second_request;      // AnmManager+0x08
    unsigned short snapshot_input;
};

static Th95PhotoRequestState th95_photo_request_state() {
    Th95PhotoRequestState state = { -1, -1, *(unsigned short*)0x004BE21E };
    void* anm = th95_anm_manager != nullptr ? *th95_anm_manager : nullptr;
    if (anm != nullptr) {
        state.first_request = *(int*)((unsigned char*)anm + 0x0C);
        state.second_request = *(int*)((unsigned char*)anm + 0x08);
    }
    return state;
}

static bool th95_same_photo_request_state(const Th95PhotoRequestState& a,
                                          const Th95PhotoRequestState& b) {
    return a.first_request == b.first_request &&
           a.second_request == b.second_request &&
           a.snapshot_input == b.snapshot_input;
}

static bool th95_ready_to_draw() {
    return th95_device() != nullptr &&
           th95_anm_manager != nullptr && *th95_anm_manager != nullptr;
}

static int __fastcall th95_skip_static_background_draw(void* self, void*) {
    (void)self;
    return 1;
}

static int __cdecl th95_startup_chain_hook() {
    th95_startup_flags_before = *(unsigned int*)(th95_game_window + 0x444);
    th95_startup_reset_flag_before = *(unsigned int*)(th95_game_window + 0x654);
    th95_startup_chain_result = ((int(__cdecl*)())0x00423D60)();
    if (th95_startup_chain_calls == 0)
        th95_startup_chain_first_result = th95_startup_chain_result;
    ++th95_startup_chain_calls;
    th95_startup_flags_after = *(unsigned int*)(th95_game_window + 0x444);
    th95_startup_reset_flag_after = *(unsigned int*)(th95_game_window + 0x654);
    return th95_startup_chain_result;
}

// The game's RunDrawChain call. The game's own pass runs the original unmodified call through this
// hook; the extra pass calls 418DA0 directly after restoring the scene snapshot.
static int __fastcall th95_render_hook(void* chain) {
    int result = th95_run_draw_chain(chain);
    th95_drew_frame = true;
    return result;
}

// thprac's th095 module uses one hook, at 0x4208D6, for GameGuiBegin, the UI update, GameGuiEnd
// and GameGuiRender. Running that hook again would advance the tool's UI at presentation rate, while
// skipping it leaves the tool out of two of every three presentations and makes it flicker.
//
// thprac's ordinary th06-th08 modules have separate update and render hooks, so ToolState can set
// GameGuiProgress back to 2 and let their normal render hook draw the cached frame again. th095 has
// only one combined hook at 0x4208D6. Its GameGuiRender also calls ImGui::Render again, which is not
// valid for th095's attach path once the frame has been rendered, so only the cached DrawData is
// passed back to thprac's renderer. The C7 layout is:
//
//     +0x02A4C0  ImGui::GetDrawData()
//     DX8: +0x08CDB0 Check, +0x08D200 RenderDrawData, device at +0x4470C0
//     DX9: +0x089640 Check, +0x0898C0 RenderDrawData, device at +0x446F90
//
// The backend is selected from the device pointer that thprac has initialized. th095's current
// thprac-C7 build uses its DX9 backend behind the game's D3D8 wrapper, while other builds can use
// DX8, so checking the source's backend constant is not sufficient. These calls repeat drawing only:
// no GameGuiBegin, input, UI update, GameGuiEnd or ImGui::Render.
static bool th95_module_name_contains(const wchar_t* name, const wchar_t* needle) {
    for (unsigned int start = 0; name[start] != L'\0'; ++start) {
        unsigned int i = 0;
        for (; needle[i] != L'\0'; ++i) {
            wchar_t a = name[start + i];
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

// thprac is injected after the game has already started, so a failed lookup is retried rather than
// cached forever. The launcher can be built under several names; the module name is what the
// shared ToolState module finder uses as well.
static unsigned char* th95_find_thprac_module() {
    static unsigned char* cached;
    static DWORD next_lookup;
    if (cached != nullptr)
        return cached;

    const DWORD now = GetTickCount();
    if (next_lookup != 0 && (LONG)(now - next_lookup) < 0)
        return nullptr;
    next_lookup = now + 1000;

    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, GetCurrentProcessId());
    if (snapshot == INVALID_HANDLE_VALUE) {
        ++th95_thprac_module_misses;
        return nullptr;
    }

    MODULEENTRY32W entry = {};
    entry.dwSize = sizeof(entry);
    if (Module32FirstW(snapshot, &entry)) {
        do {
            if (th95_module_name_contains(entry.szModule, L"thprac")) {
                cached = (unsigned char*)entry.hModule;
                break;
            }
        } while (Module32NextW(snapshot, &entry));
    }
    CloseHandle(snapshot);

    if (cached == nullptr)
        ++th95_thprac_module_misses;
    return cached;
}

// th08's thprac module reinitializes the tool at the game's device-recreation point. th095 has no
// equivalent thprac hook: both of its GUI-init hooks disable themselves after their first call, and
// THGuiCreate returns early whenever an ImGui context already exists. A windowed startup Reset would
// therefore leave thprac attached to stale backend state.
//
// C7's GameGuiInit is at VA 0x474390, i.e. RVA +0x074390 from its 0x400000 image base. Its call site
// for th095 passes impl 0, device address 0x4C4678, window address 0x4C45E8, input generation 2 and
// the two input-state addresses at 0x4BE21E/0x4BE21C. In this binary impl 0 selects the DX9 backend,
// which is also the backend the draw-only path above detects at runtime.
static bool th95_reinitialize_thprac_after_device_reset() {
    unsigned char* base = th95_find_thprac_module();
    if (base == nullptr)
        return false;

    // push ebp; mov ebp,esp; push -1; push SEH cookie
    if (base[0x074390] != 0x55 || base[0x074391] != 0x8B ||
        base[0x074392] != 0xEC || base[0x074393] != 0x6A ||
        base[0x074394] != 0xFF) {
        ++th95_thprac_guireinit_signature_mismatches;
        th95_thprac_guireinit_pending = false;
        return false;
    }

    auto game_gui_init =
        (void(__cdecl*)(int, int, int, int, int, int, int, int, float, float))(
            base + 0x074390);
    game_gui_init(0, 0x004C4678, 0x004C45E8, 2, 0x004BE21E, 0x004BE21C,
                  0, -1, 640.0f, 480.0f);
    ++th95_thprac_guireinit_count;
    th95_thprac_guireinit_pending = false;
    return true;
}

static void th95_on_device_reset(IDirect3DDevice8* device, void* caller) {
    (void)device;
    ++th95_device_reset_count;
    th95_last_device_reset_caller = caller;
    // Reset closes and replaces D3D resources while this call is still on the game's stack. The
    // th08 equivalent runs after the game's recreation routine has completed, so defer the GUI
    // setup to the next window update instead of calling into both render backends in this stack.
    th95_thprac_guireinit_pending = true;
}

// At first launch th095 benchmarks its present rate in sub_424050. OILP has already disabled the
// game's limiter and Sleep, so that benchmark sees more than 65fps, sets dword_4C4A9C, and makes
// WinMain destroy and recreate its first window before entering the forced-60 path. Set the same
// state before D3D device creation; OILP's limiter remains the pacing authority, so the benchmark
// and startup restart are unnecessary and skipping them avoids the visible flash.
static void th95_before_direct3d_create() {
    *(unsigned int*)0x004C4A9C = 1;
}

static bool th95_draw_thprac_overlay(IDirect3DDevice8* device) {
    (void)device;
    unsigned char* base = th95_find_thprac_module();
    if (base == nullptr)
        return false;

    // mov eax, [base+0x395670]
    if (base[0x02A4C0] != 0xA1 ||
        *(DWORD*)(base + 0x02A4C1) != (DWORD)(size_t)(base + 0x395670)) {
        ++th95_thprac_get_draw_signature_mismatches;
        return false;
    }
    void* thprac_device8 = *(void**)(base + 0x4470C0);
    void* thprac_device9 = *(void**)(base + 0x446F90);
    if (thprac_device8 == nullptr && thprac_device9 == nullptr) {
        ++th95_thprac_device_null;
        return false;
    }

    auto get_draw_data = (void*(__cdecl*)())(base + 0x02A4C0);

    void* draw_data = get_draw_data();
    if (draw_data == nullptr) {
        ++th95_thprac_draw_data_null;
        return false;
    }

    if (thprac_device8 != nullptr) {
        // sub esp, 28h; mov eax, [base+0x38CAC0]
        if (base[0x08CDB0] != 0x83 || base[0x08CDB1] != 0xEC ||
            base[0x08CDB2] != 0x28 || base[0x08CDB3] != 0xA1 ||
            *(DWORD*)(base + 0x08CDB4) != (DWORD)(size_t)(base + 0x38CAC0) ||
            // sub esp, 30h; mov eax, [base+0x38CAC0]
            base[0x08D200] != 0x83 || base[0x08D201] != 0xEC ||
            base[0x08D202] != 0x30 || base[0x08D203] != 0xA1 ||
            *(DWORD*)(base + 0x08D204) != (DWORD)(size_t)(base + 0x38CAC0)) {
            ++th95_thprac_dx8_signature_mismatches;
            return false;
        }

        auto check_device = (void(__cdecl*)(IDirect3DDevice8*))(base + 0x08CDB0);
        auto render_draw_data = (void(__cdecl*)(void*))(base + 0x08D200);
        check_device(device);
        render_draw_data(draw_data);
    } else {
        // mov eax, [base+0x446F90] at ImplDX9Check
        if (base[0x089640] != 0xA1 ||
            *(DWORD*)(base + 0x089641) != (DWORD)(size_t)(base + 0x446F90) ||
            // sub esp, 7Ch; mov eax, [base+0x38CAC0] at ImplDX9RenderDrawData
            base[0x0898C0] != 0x83 || base[0x0898C1] != 0xEC ||
            base[0x0898C2] != 0x7C || base[0x0898C3] != 0xA1 ||
            *(DWORD*)(base + 0x0898C4) != (DWORD)(size_t)(base + 0x38CAC0)) {
            ++th95_thprac_dx9_signature_mismatches;
            return false;
        }

        auto check_device = (void(__cdecl*)(void*))(base + 0x089640);
        auto render_draw_data = (void(__cdecl*)(void*))(base + 0x0898C0);
        check_device(thprac_device9);
        render_draw_data(draw_data);
    }
    ++th95_thprac_redraws;
    return true;
}

static void th95_extra_draw_pass() {
    IDirect3DDevice8* device = th95_device();
    void* anm = *th95_anm_manager;

    device->BeginScene();
    th95_reset_vertex_buffer(anm);
    *(unsigned int*)0x004C4DD8 = 0xFF;
    th95_disable_fog(th95_game_window);
    th95_restore_scene_state(&th95_scene);

    // The scene's original pass finished with its depth buffer populated. A repeated background
    // draw at equal depth can be rejected and leave the previous menu frame visible, so begin the
    // extra pass from the same empty depth buffer the game's own pass started with.
    device->Clear(0, nullptr, D3DCLEAR_ZBUFFER, 0, 1.0f, 0);

    ToolState::ReadyToDraw();

    // 4024A0 stores the two static background draw-chain nodes at BackGroundInf+0x2014 and
    // +0x2018. Their callbacks (402BF0/402C20) are not pure draws: they mutate layer flags,
    // background VM transforms and render state. The game's own present already contains the
    // background, so an extra frame replaces just these callbacks while the rest of the draw chain
    // still draws bullets, lasers, player and shots.
    unsigned char* background = *(unsigned char**)0x004BDD90;
    void* draw_nodes[2] = {};
    DWORD draw_callbacks[2] = {};
    if (background != nullptr) {
        draw_nodes[0] = *(void**)(background + 0x2014);
        draw_nodes[1] = *(void**)(background + 0x2018);
        for (unsigned int i = 0; i < 2; ++i) {
            if (draw_nodes[i] != nullptr) {
                draw_callbacks[i] = *(DWORD*)((unsigned char*)draw_nodes[i] + 8);
                *(DWORD*)((unsigned char*)draw_nodes[i] + 8) =
                    (DWORD)th95_skip_static_background_draw;
            }
        }
    }

    th95_run_draw_chain(th95_chain);

    for (unsigned int i = 0; i < 2; ++i) {
        if (draw_nodes[i] != nullptr)
            *(DWORD*)((unsigned char*)draw_nodes[i] + 8) = draw_callbacks[i];
    }

    th95_flush_vertex_buffer(anm);
    device->SetTexture(0, nullptr);

    // This is the draw-only half of thprac's 0x4208D6 hook. It keeps the cached UI frame on every
    // presentation without running the tool's update.
    th95_draw_thprac_overlay(device);

    device->EndScene();
}

// Stage 0 has no object movement. The scene is drawn once more from the same calculated state, so
// the photo buffer, HUD, shutter frame, enemy bullets, player and every text object remain exactly
// where the game's own presentation left them.
//
// thprac's th095 module uses one hook at 0x4208D6, and that single call starts, updates and draws
// its GUI. There is no separate frame-ready return to restore without running its update again.
// The extra pass therefore stops at the scene draw chain; ToolState is still notified at the normal
// points for the shared code, and the oilp overlay is drawn by the bypass-present path.
static void th95_extra_presentation(float t) {
    (void)t;
    // Stage 0 has no object displacement. Its additional presentation is the exact frame the game
    // already drew, so no draw-chain callback is run again. This keeps every background, menu,
    // HUD and photo draw callback from mutating state at presentation rate.
    d3d8_present_bypassing_hooks();
}

// Diagnostics. U writes oilp_diag.txt. The file uses the same 300-frame ring as the other games.
static const unsigned int th95_diag_frames_max = 300;
static const unsigned int th95_diag_presents_max = 8;
static unsigned int th95_extra_presentations;

struct Th95DiagFrame {
    LARGE_INTEGER base;
    unsigned int presents;
    unsigned int backbuffer_before;
    unsigned int backbuffer_after_game;
    unsigned int backbuffer_after_extras;
    LARGE_INTEGER present_time[th95_diag_presents_max];
};

static Th95DiagFrame th95_diag_frames[th95_diag_frames_max];
static unsigned int th95_diag_count;
static bool th95_diag_key_down;
static LARGE_INTEGER th95_diag_perf_freq;

static unsigned int th95_backbuffer_identity() {
    IDirect3DDevice8* device = th95_device();
    if (device == nullptr)
        return 0;
    IDirect3DSurface8* surface = nullptr;
    if (FAILED(device->GetBackBuffer(0, D3DBACKBUFFER_TYPE_MONO, &surface)) ||
        surface == nullptr)
        return 0;
    const unsigned int identity = (unsigned int)(size_t)surface;
    surface->Release();
    return identity;
}

static const unsigned int th95_bullet_slots = 1600;
static const unsigned int th95_bullet_slot_size = 0x65C;
static const unsigned int th95_bullet_state_offset = 0x352;
static const unsigned int th95_bullet_next_offset = 0x358;
static const unsigned int th95_bullet_layers = 6;
static const unsigned int th95_bullet_layer_heads_offset = 0x04;
static const unsigned int th95_bullet_count_offset = 0x27C5B4;

static unsigned int th95_count_bullet_chain(unsigned char* manager) {
    unsigned int count = 0;

    for (unsigned int layer = 0; layer < th95_bullet_layers; ++layer) {
        unsigned char* bullet =
            *(unsigned char**)(manager + th95_bullet_layer_heads_offset + layer * 4);
        while (bullet != nullptr && count < th95_bullet_slots) {
            ++count;
            bullet = *(unsigned char**)(bullet + th95_bullet_next_offset);
        }
    }
    return count;
}

static unsigned int th95_count_active_bullet_slots(unsigned char* manager) {
    unsigned int count = 0;
    for (unsigned int i = 0; i < th95_bullet_slots; ++i) {
        unsigned char* bullet = manager + 0x4C + (size_t)i * th95_bullet_slot_size;
        if (*(unsigned short*)(bullet + th95_bullet_state_offset) != 0)
            ++count;
    }
    return count;
}

static void th95_diag_dump() {
    wchar_t path[1024] = {};
    if (!GetModuleFileNameW(NULL, path, MAX_PATH))
        return;
    wchar_t* slash = wcsrchr(path, L'\\');
    if (slash)
        slash[1] = L'\0';
    wcscat_s(path, L"oilp_diag.txt");

    FILE* file = nullptr;
    if (_wfopen_s(&file, path, L"w") != 0 || file == nullptr)
        return;

    unsigned int count = th95_diag_count < th95_diag_frames_max
                             ? th95_diag_count
                             : th95_diag_frames_max;
    unsigned int start = th95_diag_count < th95_diag_frames_max
                             ? 0
                             : th95_diag_count % th95_diag_frames_max;

    fprintf(file, "oilp frame interpolation diagnostics\n");
    fprintf(file, "logic frame rate: %.2f, configured presentation rate: %.2f, display refresh: %uHz\n",
            Limiter::game_fps, Limiter::PresentRate(), Limiter::DisplayRefresh());
    fprintf(file, "configured game frame rate: %u, BltPrepareTime %u, sleep %d\n",
            Config::GameFPS, Config::BltPrepareTime, (int)Config::Sleep);
    fprintf(file, "frames recorded: %u\n", count);

    // There is no persistent FPS string in this game: 423790 formats directly into a local text
    // object. Rebuild the same text with the format string at 0x495E24 for comparison.
    {
        char fps_text[32] = {};
        const float fps = *(float*)0x004C4E0C;
        sprintf_s(fps_text, sizeof(fps_text), "%2.1ffps", fps);
        fprintf(file, "game's own frame rate: %.2f, its text (same format): \"%s\", count: %u\n",
                fps, fps_text, *(unsigned int*)0x004C4DF8);
    }

    {
        const double numerator = *(double*)0x004C4DFC;
        const double denominator = *(double*)0x004C4E04;
        const double ratio = denominator != 0.0 ? numerator / denominator : 0.0;
        fprintf(file, "game's own slow-rate ratio: numerator %.4f, denominator %.4f, "
                      "recorded percent %.3f\n",
                numerator, denominator, denominator != 0.0 ? (1.0 - ratio) * 100.0 : 0.0);
    }
    fprintf(file, "frameskip config: %u\n", (unsigned int)*(unsigned char*)0x004C483C);
    {
        const D3D8Hook::PresentInfo present = D3D8Hook::GetPresentInfo();
        fprintf(file,
                "D3D8 present: %ux%u, back buffers %u, swap effect %u, interval 0x%08x, "
                "windowed %d\n",
                present.width, present.height, present.back_buffer_count,
                present.swap_effect, present.presentation_interval, present.windowed ? 1 : 0);
    }

    {
        unsigned char* manager = *(unsigned char**)0x004BDD98;
        if (manager != nullptr) {
            fprintf(file, "bullets: %u on draw chains, %u active slots, manager count %u, "
                          "0 projected forward (stage 0)\n",
                    th95_count_bullet_chain(manager),
                    th95_count_active_bullet_slots(manager),
                    *(unsigned int*)(manager + th95_bullet_count_offset));
        } else {
            fprintf(file, "bullets: BulletInf is not available\n");
        }
    }

    // Extra draws invoke only RunDrawChain. The screenshot copy is inside the game-present wrapper,
    // and PhotoInf::OnUpdate is a calc-chain callback. Both paths are absent from the extra path.
    // The request ids are checked around every extra to catch accidental photo state changes.
    {
        const Th95PhotoRequestState photo = th95_photo_request_state();
        fprintf(file,
                "photo: %u extra presentations, %u request-state changes, %u game-present calls "
                "from extras, 0 PhotoInf updates in extras, 0 screenshot-copy calls in extras\n",
                th95_extra_presentations, th95_photo_request_changes,
                th95_extra_game_present_calls);
        fprintf(file, "photo request ids: %d/%d, snapshot key: %s (0x%04x)\n",
                photo.first_request, photo.second_request,
                (photo.snapshot_input & 0x800) != 0 ? "down" : "up",
                (unsigned int)photo.snapshot_input);
    }
    fprintf(file,
            "thprac draw-only: %u redraws, %u module misses, %u get-draw signature mismatches, "
            "%u DX8 signature mismatches, %u DX9 signature mismatches, "
            "%u device-null, %u draw-data-null, %u GUI reinitializations, "
            "%u GUI reinitialization signature mismatches, %u D3D8 resets, "
            "last reset caller 0x%08x\n",
            th95_thprac_redraws, th95_thprac_module_misses,
            th95_thprac_get_draw_signature_mismatches,
            th95_thprac_dx8_signature_mismatches,
            th95_thprac_dx9_signature_mismatches,
            th95_thprac_device_null, th95_thprac_draw_data_null,
            th95_thprac_guireinit_count,
            th95_thprac_guireinit_signature_mismatches,
            th95_device_reset_count, (unsigned int)(size_t)th95_last_device_reset_caller);
    {
        const WindowMode::EarlyWindowStats window = WindowMode::GetEarlyWindowStats();
        fprintf(file,
                "window early: %u CreateWindowExA calls, %u main-window calls, "
                "%u applications, last return address 0x%08x\n",
                window.create_calls, window.main_window_calls, window.applications,
                (unsigned int)(size_t)window.last_return_address);
    }
    fprintf(file,
            "startup chain: %u calls, first result %d, last result %d, "
            "flags 0x%08x -> 0x%08x, "
            "reset flag %u -> %u\n",
            th95_startup_chain_calls, th95_startup_chain_first_result,
            th95_startup_chain_result,
            th95_startup_flags_before, th95_startup_flags_after,
            th95_startup_reset_flag_before, th95_startup_reset_flag_after);
    fprintf(file, "window updates: %u calls, %u nonzero, last result %d\n",
            th95_window_update_calls, th95_window_update_nonzero,
            th95_last_window_update_result);
    double presents = 0.0;
    double first = 0.0;
    double last = 0.0;
    for (unsigned int i = 0; i < count; ++i) {
        const Th95DiagFrame& frame =
            th95_diag_frames[(start + i) % th95_diag_frames_max];
        fprintf(file, "frame %4u: %u present(s), backbuffers 0x%08x -> 0x%08x -> 0x%08x",
                i, frame.presents, frame.backbuffer_before, frame.backbuffer_after_game,
                frame.backbuffer_after_extras);
        for (unsigned int p = 0; p < frame.presents && p < th95_diag_presents_max; ++p) {
            fprintf(file, " [%u] %.2fms", p,
                    (double)(frame.present_time[p].QuadPart - frame.base.QuadPart) * 1000.0 /
                        (double)th95_diag_perf_freq.QuadPart);
        }
        fprintf(file, "\n");
        presents += frame.presents;
        if (i == 0)
            first = (double)frame.base.QuadPart;
        last = (double)frame.base.QuadPart;
    }
    if (last > first && count > 0) {
        fprintf(file, "average: %.3f presents per frame, %.1f presentations per second\n",
                presents / (double)count,
                presents * (double)th95_diag_perf_freq.QuadPart / (last - first));
    }

    fclose(file);
    printf("Wrote oilp_diag.txt\n");
}

// 4208FF calls the game's complete present wrapper. Running that wrapper first keeps the shutter,
// snapshot file and photo texture copies ahead of every extra pass. All extras then use the D3D8
// bypass present so tools that measure one Present per game frame do not see the interpolated
// presents.
static int th95_present_hook() {
    if (th95_diag_perf_freq.QuadPart == 0)
        QueryPerformanceFrequency(&th95_diag_perf_freq);

    if (th95_in_extra_presentation)
        ++th95_extra_game_present_calls;

    LARGE_INTEGER base;
    QueryPerformanceCounter(&base);
    const unsigned int backbuffer_before = th95_backbuffer_identity();
    th95_drew_frame = true;

    int result = th95_game_present();
    const unsigned int backbuffer_after_game = th95_backbuffer_identity();

    Limiter::MarkFrameWorkEnd();
    ToolState::WatchFrameDrawn();

    LARGE_INTEGER present_time[th95_diag_presents_max] = {};
    unsigned int presents = 1;
    present_time[0] = base;

    if (th95_drew_frame && !Limiter::PresentingEveryNthFrame() && Limiter::ShouldPresent()) {
        const double rate = Limiter::PresentRate();
        LARGE_INTEGER spacing;
        const unsigned int extras = Limiter::ExtraPresentations(rate, spacing);

        LARGE_INTEGER frame_start = Limiter::FrameStart();
        LARGE_INTEGER frame_wait = Limiter::FrameWait();
        LARGE_INTEGER game_work;
        game_work.QuadPart = base.QuadPart - frame_start.QuadPart;

        if (extras > 0 && spacing.QuadPart > 0) {
            const bool has_deadline = frame_start.QuadPart > 0 && game_work.QuadPart > 0;
            LARGE_INTEGER deadline;
            deadline.QuadPart = frame_start.QuadPart + frame_wait.QuadPart;

            for (unsigned int k = 1; k <= extras; ++k) {
                LARGE_INTEGER target;
                target.QuadPart = base.QuadPart + spacing.QuadPart * (LONGLONG)k;
                if (has_deadline && target.QuadPart + game_work.QuadPart > deadline.QuadPart)
                    continue;

                Limiter::WaitUntil(target);
                if (presents < th95_diag_presents_max)
                    QueryPerformanceCounter(&present_time[presents]);
                ++presents;
                ++th95_extra_presentations;

                th95_extra_presentation((float)k / (float)(extras + 1));
            }
        }
    }

    const unsigned int backbuffer_after_extras = th95_backbuffer_identity();
    th95_drew_frame = false;

    {
        Th95DiagFrame& frame = th95_diag_frames[th95_diag_count % th95_diag_frames_max];
        frame.base = base;
        frame.presents = presents;
        frame.backbuffer_before = backbuffer_before;
        frame.backbuffer_after_game = backbuffer_after_game;
        frame.backbuffer_after_extras = backbuffer_after_extras;
        for (unsigned int p = 0; p < th95_diag_presents_max; ++p)
            frame.present_time[p] = p < presents ? present_time[p] : present_time[0];
        ++th95_diag_count;
    }

    if (GetAsyncKeyState('U') & 0x8000) {
        if (!th95_diag_key_down) {
            th95_diag_key_down = true;
            th95_diag_dump();
        }
    } else {
        th95_diag_key_down = false;
    }

    return result;
}

static void th95_install_interpolation() {
    {
        // 4208AE: call 418DA0, RunDrawChain. Stage 0 keeps this hook installed because th095's
        // startup/render flow depends on the same call shape, but it performs no state capture.
        BYTE* site = (BYTE*)0x004208AE;
        if (site[0] == 0xE8 &&
            *(DWORD*)(site + 1) == 0x00418DA0 - (0x004208AE + 5)) {
            patch_call(0x004208AE, th95_render_hook);
        } else {
            printf("th95 draw chain call at 0x4208AE doesn't look like a known one, skipping!\n");
        }
    }
    {
        // 4208FF: call 420920, the game's Present, screenshot and snapshot wrapper.
        BYTE* site = (BYTE*)0x004208FF;
        if (site[0] == 0xE8 &&
            *(DWORD*)(site + 1) == 0x00420920 - (0x004208FF + 5)) {
            patch_call(0x004208FF, th95_present_hook);
        } else {
            printf("th95 present call at 0x4208FF doesn't look like a known one, skipping!\n");
        }
    }

    overlay_show_present_rate(true);

    // The actual presentation rate is counted by the D3D8 Present path. ShowOverlay still decides
    // whether the overlay exists; this only enables the rate line when the overlay is drawn.
    printf("Frame interpolation: th95 stage 0 extra presentations installed (setting %d)\n",
           Config::Interpolation);
}

int __fastcall th95_window_update_hook(void* self) {
    IDirect3DDevice8* device = th95_device();
    if (device != nullptr) {
        D3D8Hook::Install(device);
        if (th95_thprac_guireinit_pending)
            th95_reinitialize_thprac_after_device_reset();
    }

    // thprac and dinput8 proxies inject this DLL during process initialization, while the
    // standalone oilp loader injects it immediately before the entrypoint. Installing the new
    // Render hooks during process initialization leaves th095 stuck before its main loop. Defer
    // them until the first window update, after the game's globals and D3D device exist.
    if (!th95_interpolation_install_attempted) {
        th95_interpolation_install_attempted = true;
        if (Config::Interpolation != 0 || Config::InterpolationFPS != 0) {
            th95_install_interpolation();
        }
    }

    Limiter::Tick();
    th95_last_window_update_result = CWindowManager__Update(self);
    ++th95_window_update_calls;
    if (th95_last_window_update_result != 0)
        ++th95_window_update_nonzero;
    return th95_last_window_update_result;
}

void th95_install_patches() {
    D3D8Hook::SetDeviceResetCallback(th95_on_device_reset);
    D3D8Hook::SetDirect3DCreateCallback(th95_before_direct3d_create);

    {
        // 4203E0: call 423D60, the first calculation-chain step of WinMain.
        BYTE* site = (BYTE*)0x004203E0;
        if (site[0] == 0xE8 &&
            *(DWORD*)(site + 1) == 0x00423D60 - (0x004203E0 + 5)) {
            patch_call(0x004203E0, th95_startup_chain_hook);
        } else {
            printf("th95 startup chain call at 0x4203E0 doesn't look like a known one, "
                   "skipping diagnostics!\n");
        }
    }

    {
        // Skip the original frame limiter
        BYTE patch[] = { 0xEB };
        patch_bytes(0x004207DA, patch, sizeof(patch));
    }
    {
        // Don't sleep in the main loop
        BYTE patch[] = { 0x90, 0x90, 0x90, 0x90, 0x90, 0x90 };
        patch_bytes(0x004207C3, patch, sizeof(patch));
    }
    {
        // Hook window update
        patch_call(0x0042046F, th95_window_update_hook);
    }
}

FPSTarget th95_replay_callback() {
    if (*CReplayManager::InstancePtr && (*CReplayManager::InstancePtr)->mode == 1) {
        // TODO: Reverse engineer this struct instead of being lazy
        auto input = *(DWORD*)0x004BE218;
        if (input & InputState::Focus)
            return FPSTarget::ReplaySlow;
        else if (input & (InputState::Skip | InputState::Shoot))
            return FPSTarget::ReplaySkip;
    }
    return FPSTarget::Game;
}
