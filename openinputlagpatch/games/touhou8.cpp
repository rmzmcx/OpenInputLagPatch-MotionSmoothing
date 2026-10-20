// Touhou 8: Imperishable Night v1.00d

#include <stdio.h>
#include <math.h>
#include <string.h>
#include "patch_util.h"
#include "d3d8_hook.h"
#include "limiter.h"
#include "overlay.h"
#include "tool_state.h"
#include "touhou8.h"
#include "config.h"

using namespace Touhou8;

CEngine* CEngine::Instance = (CEngine*)0x017CE758;
CGame* CGame::Instance = (CGame*)0x0160F508;

int __fastcall th8_window_update_hook(void* self) {
    Limiter::Tick();
    return CWindowManager__Update(self);
}

// ---------------------------------------------------------------------------
// Input handling, ported from vpatch (see the notes for the whole trail).
//
// th08 creates its keyboard device with SetCooperativeLevel(hwnd, 0x16) (0x446B81 / 0x446B98),
// which is DISCL_NOWINKEY | DISCL_FOREGROUND | DISCL_NONEXCLUSIVE: while the game window is not the
// foreground window the device is unacquired and its state stops being updated, so a key held when
// the focus was lost and released while away comes back still reading as held. The game never
// re-syncs the state when it is activated again (WM_ACTIVATEAPP at main.cpp:501 only touches the
// cursor flags, Controller::ResetKeyboard is startup-only, and the devices are only released on
// exit), so the key stays stuck until it is pressed again.
//
// vpatch installs four patches for this, unconditionally in its th08 build, and these are those
// four, at the same addresses:
//
//   1. the call to Controller::GetInput inside Supervisor::OnUpdate (0x4455F0, right after
//      g_LastFrameInput = g_CurFrameInput) is redirected into the wrapper below;
//   2. the function that ends at 0x452D52 (the one that fills the game's state copy) gets its ret
//      replaced by a store, so returning from it raises the flag;
//   3. 0x452319, at the top of the delayed-input-copy updater, spends the flag: the function
//      returns 1 straight away for that frame instead of updating the copy;
//   4. 0x452326's branch target moves from 0x452332 to 0x45234E, so the copy work is skipped in
//      the case that patch 2's flag is not the one being handled.
//
// Note what the wrapper does and does not do: it keeps 0x164D52C / 0x164D534 (the input copy and
// its previous frame, which the pause/menu edge detection reads) under its own control, with the
// Pause bit (bit 3) filtered out and a blanked frame when the flag is up, but it always returns the
// real input to the game logic. Nothing here touches the DirectInput device itself, so this is
// vpatch's own fix for its own symptom, not a general stale-device fix.
// ---------------------------------------------------------------------------
static DWORD th8_input_discard;

// The game's own flags word, read the way the instruction the consumer below replaces (0x452319,
// which tests bit 2 for "not in a menu") read it. A C read on purpose: in inline asm this compiler
// turns "mov eax, dword ptr [0164D0B4h]" into "mov eax, 164D0B4h", the number itself, which is not
// the game's state at all (0x164D0B4 happens to have bit 2 set, so it went unnoticed here, but
// Touhou 7's value has it clear and its replay recorder was dead because of that - see
// th7_game_flags in touhou7.cpp). Lesson: never hand an absolute address to an inline asm memory
// operand here; use a C read.
static unsigned int th8_game_flags() {
    return *(volatile unsigned int*)0x0164D0B4;
}

// Where the game calls Controller::GetInput (0x4455F0), vpatch's 60003DE0
static __int16 th8_input_hook() {
    __int16 input = get_input();
    if ((th8_game_flags() & 8) == 0) {
        __int16 copy = input;
        if (th8_input_discard != 0)
            copy = 0;
        *(volatile WORD*)0x0164D534 = *(volatile WORD*)0x0164D52C;
        *(volatile WORD*)0x0164D52C = (WORD)(copy & 0xFFF7);
    }
    return input;
}

// The ret at 0x452D52 of the function that fills the game's state copy (vpatch stores the flag
// from the value the game happens to have in edx there)
static __declspec(naked) void th8_input_flag_setter() {
    __asm { mov dword ptr [th8_input_discard], edx }
    __asm { ret }
}

// 0x452319, the top of the delayed-input-copy updater: spend the flag (vpatch's 60003E20)
static __declspec(naked) void th8_input_flag_consumer() {
    __asm { mov eax, dword ptr [th8_input_discard] }
    __asm { test eax, eax }
    __asm { jne spend }
    // eax = the flags the game's own instruction would have read, then the game's next instruction
    __asm { call th8_game_flags }
    __asm { mov ecx, 0045231Eh }
    __asm { jmp ecx }
spend:
    __asm { xor eax, eax }
    __asm { mov dword ptr [th8_input_discard], eax }
    __asm { mov ecx, 0045247Bh }
    __asm { jmp ecx }
}

// Takes over the game's present call and the draw chain call that precedes it (defined below,
// after the comment that explains what a frame is made of)
static void th8_install_interpolation();
// Replaces the frame rate the game measures for itself (see the comment at its definition)
static void th8_install_fps_measurement();

void th8_install_patches() {
    {
        // Skip the original frame limiter
        BYTE patch[] = { 0xEB };
        patch_bytes(0x00441EE6, patch, sizeof(patch));
    }
    {
        // Don't sleep in the main loop
        BYTE patch[] = { 0x90, 0x90, 0x90, 0x90, 0x90, 0x90 };
        patch_bytes(0x00441ECF, patch, sizeof(patch));
    }
    {
        // Hook window update
        patch_call(0x00441B15, th8_window_update_hook);
    }
    {
        // vpatch's input handling for this game (see the note above), applied the way vpatch
        // applies it: always, not behind a config option. It is what takes this game's frame of
        // input lag out, and it is also what keeps keys from being left held when the window
        // loses focus - a vpatched game has both, so this one has to as well.
        {
            // The call to Controller::GetInput in Supervisor::OnUpdate
            patch_call(0x004455F0, th8_input_hook);
        }
        {
            // The updater's flag check: return early instead of updating the copy
            patch_jmp(0x00452319, th8_input_flag_consumer);
        }
        {
            // Its branch target moves one step on, so the copy work is skipped
            BYTE patch[] = { 0x75, 0x26 };
            patch_bytes(0x00452326, patch, sizeof(patch));
        }
        {
            // The function that fills the state copy raises the flag where it returns
            patch_jmp(0x00452D52, th8_input_flag_setter);
        }
    }
    if (Config::Interpolation != 0 || Config::InterpolationFPS != 0)
        th8_install_interpolation();
    if (Config::CalcFPS)
        th8_install_fps_measurement();
}

// ---------------------------------------------------------------------------
// Frame interpolation, stage 0: extra presentations
// ---------------------------------------------------------------------------
//
// A frame of this game, in the order GameWindow::Render (0x441E70..0x442051) runs it:
//
//     0x441F07  viewport = { 0, 0, 640, 480 } / device->SetViewport
//     0x441F48  call 0x43CA50    Chain::RunCalcChain - the input is read at its head, by
//                                Supervisor::OnUpdate (chain priority 0)
//     0x441FB4  device->BeginScene
//     0x441FC8  AnmManager::ClearVertexBuffer        (ECX = [0x18BDC90])
//     0x441FD3  [0x17CEAA8] = 0xFF                   g_Supervisor.fogState = FOG_UNSET
//     0x441FDD  call 0x448229    Supervisor::DisableFog               (ECX = 0x17CE758)
//     0x441FEC  call 0x43CB60    Chain::RunDrawChain - everything on screen is drawn here
//     0x441FF7  AnmManager::FlushVertexBuffer
//     0x44200E  device->SetTexture(0, NULL)
//     0x442022  device->EndScene
//     0x44203D  call 0x442060    GameWindow::Present
//     0x442069  device->Present  (through the hook the patch has on the device)
//                ... and then AnmManager::TakeScreencaptures reads back buffer 0 back, which the
//                afterimage effects and the pause screen background draw from
//
// The block from 0x441FB4 to 0x442027 is what draws the frame, and 0x44203D is where it reaches the
// display - so an extra presentation is that block run again and presented again, right after the
// game's own present, spread over what is left of the frame. That is the time the game would
// otherwise spend sitting in the limiter waiting for the next frame.
//
// Two things that happen inside that block have to be kept out of the extras:
//
//   - The game counts its own frame rate off the draw pass. Supervisor::DrawFpsCounter (0x445BC0,
//     a draw chain callback) calls Supervisor::CalculateFps (0x446F53) with its "draw it" argument
//     set, and that counts the pass (0x17CF4C0 += frameskipConfig + 1, 0x446F72..0x446F7C), keeps a
//     500ms window over it and works the game's own frame rate out of it. The rate it works out is
//     drawn in the bottom right corner ("%.02ffps" formatted into 0x17CF3B8, 0x447031..0x44703B)
//     and is what the game's frame rate correction is driven by (Supervisor::lagNumerator at
//     0x17CE8E8 and the rounded rate at 0x17CE8F0, both written by the same function). Run once
//     per extra presentation it would count three passes per frame, and the game would read its own
//     frame rate as three times what it is and correct for that. The state that function works in
//     is put back around every extra pass (th8_fps_state below), which leaves the game's accounting
//     exactly where its own pass left it and makes the extras draw the same frame rate text that
//     pass drew.
//   - The game's own frame skip (g_Supervisor.cfg.frameskipConfig <= framesSinceRedraw, 0x441FB2)
//     can leave a frame with no draw pass at all: it then presents the picture it already had. The
//     extras have nothing of their own to draw in that frame, so they are left out of it (see
//     th8_drew_frame) and the game's own present simply runs at its own rate.
//
// On text, this game's draw passes are self-contained: the string list everything draws through is
// drawn and then cleared inside the same pass (the low priority ascii pass runs
// AsciiManager::OnDrawLowPrioImpl and then ResetStrings, AsciiManager.cpp 189..195), and what
// appends to it (the HUD, Gui.cpp 1227 and on, at chain priority 17 against the ascii pass' 20)
// runs earlier in that same pass. So an extra pass draws the same text the game's own pass drew
// instead of stacking a copy of it on top, which is what th12 to th20 had to deal with here.

// The device the frame is drawn with and presented through. The game reads it from here itself
// (0x441FB4, 0x442069): g_Supervisor.d3dDevice.
static IDirect3DDevice8* th8_device() {
    return *(IDirect3DDevice8**)0x017CE760;
}

// The game's own calls the extra presentations make, at the addresses the game calls them from (see
// the frame above). The draw chain takes the supervisor's critical section itself, so it can be run
// from inside the frame the same way the game runs it.
static void** th8_anm_manager = (void**)0x018BDC90; // g_AnmManager
static void* th8_supervisor = (void*)0x017CE758;    // g_Supervisor
static void* th8_chain = (void*)0x0164F548;         // g_Chain
static DWORD* th8_fog_state = (DWORD*)0x017CEAA8;   // g_Supervisor.fogState

static auto th8_clear_vertex_buffer = (void(__fastcall*)(void*))0x00462E00; // 0x441FC8
static auto th8_flush_vertex_buffer = (void(__fastcall*)(void*))0x00462E40; // 0x441FF7
static auto th8_disable_fog = (int(__fastcall*)(void*))0x00448229;          // 0x441FDD
static auto th8_run_draw_chain = (int(__fastcall*)(void*))0x0043CB60;       // 0x441FEC
static auto th8_game_present = (void(__fastcall*)(void*))0x00442060;        // 0x44203D

// What the scene is drawn with, taken off the device where the game's own draw pass starts (the
// viewport is set to the play field while the stage is drawn and put back by the HUD, and the menus
// change it too, so an extra presentation has to put back whatever the frame it repeats used)
static D3DVIEWPORT8 th8_scene_viewport = {};
static D3DMATRIX th8_scene_view = {};
static D3DMATRIX th8_scene_projection = {};
static bool th8_scene_transforms = false;

// The screen shake, AnmManager::screenShakeOffset (a Float2 at AnmManager+0x1C, written by
// ScreenEffect::CalcShake 0x45BDC0 and read by every 2D draw, AnmManager.cpp:995..1002), taken the
// same way and for the same reason.
//
// It is set in the calculation chain and cleared *inside* the draw pass: the HUD's ascii pass zeroes
// it once the game has drawn everything it is meant to shake (AsciiManager.cpp:1773). So by the time
// the game's own pass ends the value is gone, and an extra pass that does not put it back draws the
// play field, the bullets, the enemies and the player without any shake at all. With three
// presentations per frame that is one shaken picture and two still ones, which reads as the shake
// having a third of its amplitude - exactly how the th08 screen shake was reported as weakened.
//
// th06 needs nothing like this: its ScreenEffect_ShakeScreen moves the play field's own rectangle
// (GameManager::gameRegionScreenPos), which the draw pass never clears. th07 has the same pattern as
// this game and already puts its AnmManager offset back (games/touhou7.cpp, th7_scene_offset).
static float th8_scene_shake[2] = {};

// Whether the game drew this frame (see the frame skip note above)
static bool th8_drew_frame = false;

// The AnmManager's cached copy of the texture it last bound (AnmManager+0x24BC, compared against
// the sprite's texture at 0x464CCB in AnmManager::DrawInner and only re-bound when they differ).
//
// An extra presentation draws through the same manager, and the pass it repeats ended by setting
// the device's texture to NULL (0x44200E, the game's own last step) - and, with ShowOverlay on, by
// the overlay binding its own font texture. Either way the device no longer holds what that cache
// says it holds, and since the manager only calls SetTexture when the cache says it has to, the
// first sprites the extra draws would come out with whatever was left bound: the frame rate text
// and the loading screen's text end up as plain white quads with the overlay off, and as pieces of
// the overlay's font atlas (text that looks smeared) with it on. Forgetting the cache makes the
// pass bind what it draws, exactly the way the game's own pass does when it starts a frame.
//
// (0x18BDC90 holds the manager the game itself draws through - see th8_anm_manager.)
static void th8_forget_anm_texture() {
    if (*th8_anm_manager != nullptr)
        *(void**)((char*)*th8_anm_manager + 0x24BC) = nullptr;
}

// The state Supervisor::CalculateFps (0x446F53) works in. It is called from the draw chain (see the
// comment above), so every extra presentation runs it once more, and this is what puts the game's
// accounting back the way its own pass left it:
//
//     0x17CF4C0  the count of draw passes in the current window (0x446F72, 0x446F7C, 0x446FCE)
//     0x17CF4B8  when that window started and 0x17CF4BC whether it has one (0x446F8E..0x447008)
//     0x17CF3B8  the "%.02ffps" text that count is drawn as (sprintf at 0x44703B)
//     0x17CF2B8  the replay speed text ("%2d", sprintf at 0x447137)
//     0x17CF2A8..0x17CF2B4  the state that the replay path of the counter keeps
//     (0x447149..0x4471DF; the branch it is on is dead in this build - 0x17CEA58 is only ever read)
//     0x17CE8E8  Supervisor::lagNumerator, the lag the game compensates its frame rate for
//     0x17CE8EC  Supervisor::lagDenominator, the other half of that measurement: the two are the
//     game's own "how much of the frame rate was lost" ratio (every consumer divides the first by
//     this one - SaveReplay 0x453676, ResultScreen::DrawFinalStats 0x458183, the replay list
//     0x4563E5 - and its score/rank and the replay's recorded slowdown rate come out of it). The
//     function adds 60.0 to it per draw pass (0x447069..0x447072) and 30.0 to 60.0 to the numerator,
//     so a pass the extras run without putting this back skews the ratio: that is what "the
//     processing rate went wrong" was (the extras added two passes' worth per frame, and only the
//     numerator was being put back).
//     0x17CE8F0  Supervisor::unk198, the rounded frame rate it works out (0x447122)
//
// The whole 0x17CF2A0..0x17CF4C4 block holds only those (nothing else in the executable reads or
// writes any of it), and the supervisor fields sit outside it, so that is what is saved and put
// back. The list of what the function writes is short enough to check: every absolute address it
// stores to is in the block above, or one of 0x17CE8E8, 0x17CE8EC and 0x17CE8F0.
struct Th8FpsState {
    unsigned char block[0x224];
    float lag_numerator;
    float lag_denominator;
    short rounded_rate;
};

static void th8_fps_state_save(Th8FpsState* state) {
    memcpy(state->block, (void*)0x017CF2A0, sizeof(state->block));
    state->lag_numerator = *(float*)0x017CE8E8;
    state->lag_denominator = *(float*)0x017CE8EC;
    state->rounded_rate = *(short*)0x017CE8F0;
}

static void th8_fps_state_restore(const Th8FpsState* state) {
    memcpy((void*)0x017CF2A0, state->block, sizeof(state->block));
    *(float*)0x017CE8E8 = state->lag_numerator;
    *(float*)0x017CE8EC = state->lag_denominator;
    *(short*)0x017CE8F0 = state->rounded_rate;
}

// ---------------------------------------------------------------------------
// The frame rate the game measures for itself (vpatch's CalcFPS)
//
// Touhou 8 works its own frame rate out in Supervisor::CalculateFps (0x446F53): every frame it adds
// 1 + frameskipConfig to the counter at 0x17CF4C0, and once half a second has gone by it divides
// that counter by the time the window took and clears it. That reading decides how much of the half
// second the game counts as lost, which it accumulates into 0x17CE8E8 over 0x17CE8EC
// (lagNumerator / lagDenominator) - the pair the results screen's processing rate
// (ResultScreen.cpp:2016) and a replay's recorded value (SaveReplay, 0x453464) come out of. A
// window that a load or a scene change lands in reads as a slow one, and the run carries that as
// lost frame rate from then on.
//
// vpatch's CalcFPS=1 takes the measurement over: its patch at 0x446F6B skips the game's own
// counting, timing and division and carries on at 0x447029 with the value its stub 60003DC0 worked
// out. This does the same with the frame rate the limiter measured for itself (see
// Limiter::CountGameFrame).
// ---------------------------------------------------------------------------
static void* th8_fps_continue(void* frame) {
    // The frame pointer this is handed is the game's own; a hook that gets the calling convention
    // wrong hands over something else entirely, which used to mean writing through it. Writing only
    // to something that can be a stack address keeps that mistake from becoming memory corruption.
    if ((size_t)frame < 0x10000)
        return (void*)0x00447029;
    // The counter the game adds to is left where its own code would have left it
    *(unsigned int*)0x017CF4C0 = 0;
    *(float*)((char*)frame - 4) = Limiter::MeasuredFps();
    // Where vpatch continues: the game formats and draws the frame rate text from here, and the
    // per half second weighting (0x447043 on) runs as the game wrote it
    return (void*)0x00447029;
}

static __declspec(naked) void th8_fps_hook() {
    // The game's frame pointer, pushed the way the game's own calls pass their arguments (it is
    // what the fps variable is written through, see th8_fps_continue)
    __asm { push ebp }
    __asm { call th8_fps_continue }
    __asm { add esp, 4 }
    __asm { jmp eax }
}

static void th8_install_fps_measurement() {
    // 0F B6 05 93 E8 7C 01 (movzx eax, byte [0x17CE893], the frameskip config the counter starts
    // from)
    BYTE* site = (BYTE*)0x00446F6B;
    if (site[0] == 0x0F && site[1] == 0xB6 && site[2] == 0x05 &&
        *(DWORD*)(site + 3) == 0x017CE893) {
        patch_jmp(0x00446F6B, th8_fps_hook);
    } else {
        printf("th08 fps measurement at 0x446F6B doesn't look like a known one, skipping!\n");
    }
}

// Where the user's tool builds its frame and where it draws it (see tool_state.h). thprac's th08
// module hooks the return of Chain::RunCalcChain (0x43CB37) with its "th08_update" hook to build
// its overlay frame and the return of Chain::RunDrawChain (0x43CC45) with its "th08_render" hook to
// draw it; both are plain `ret`s, so the defaults are right for them.
static unsigned char* th8_tool_frame_address = (unsigned char*)0x0043CB37;
static unsigned char* th8_tool_draw_address = (unsigned char*)0x0043CC45;

// How far along the extra presentation being drawn sits, as a fraction of a frame; 0 while the
// game's own pass is being drawn (see th8_extra_presentation)
static float th8_extra_t;

// ---------------------------------------------------------------------------
// Frame interpolation, stage 1: the player
// ---------------------------------------------------------------------------
//
// The player's sprite is not drawn where the player is: Player::OnDrawHighPrio (0x44D530) writes the
// sprite VM's own position from the player's first - `[player+0x218] = [player+0x2B4] + the screen
// shake`, `[player+0x21C] = [player+0x2B8] + the same` (0x44D568..0x44D59C) - and then draws the VM
// at player+0x10 (0x44D5AF..0x44D5B9). So that VM position is what an extra presentation has to
// move, and it has to be moved after the game's own code has written it, which is the call that
// draws the sprite: it is taken over here, the position is nudged by however far the player moves in
// a frame times the extra's fraction of it, the sprite is drawn, and the position goes back.
//
// How far the player moves in a frame is measured rather than read: the player's movement adds its
// velocity (player+0x3F8) to its position once a frame, but it is not the only thing that can move
// it (a death or a stage transition repositions it outright), so the position is sampled once a
// frame - in the game's own pass, at the top of the draw chain - and the step is the difference to
// what it was the frame before. Nothing of the player's own state is written anywhere but that VM
// position, which is put back the moment the sprite is drawn.

static unsigned char* th8_player = (unsigned char*)0x017D5EF8; // g_Player
static const unsigned int th8_player_pos_offset = 0x2B4;
static const unsigned int th8_player_sprite_pos_offset = 0x218; // the VM at player+0x10, pos at +0x208

static float th8_player_last_pos[2];
static float th8_player_step[2];
static bool th8_player_pos_known;

// What the player moved by over the last frame, for the diagnostics
static float th8_player_step_length;

// Called once a frame, from the frame's first draw pass (see th8_render_hook)
static void th8_player_measure() {
    const float* pos = (const float*)(th8_player + th8_player_pos_offset);

    if (th8_player_pos_known) {
        th8_player_step[0] = pos[0] - th8_player_last_pos[0];
        th8_player_step[1] = pos[1] - th8_player_last_pos[1];
        th8_player_step_length = (float)sqrt((double)(th8_player_step[0] * th8_player_step[0] +
                                                      th8_player_step[1] * th8_player_step[1]));
    } else {
        th8_player_step[0] = 0.0f;
        th8_player_step[1] = 0.0f;
        th8_player_pos_known = true;
    }

    th8_player_last_pos[0] = pos[0];
    th8_player_last_pos[1] = pos[1];
}

// The draw the hook goes in front of (the call at 0x44D5B9) is shaped like the AnmManager's own
// draws: the manager in ECX, and the VM the caller pushed (0x44D5B2) as its one argument - which
// the function itself takes off again (0x4631C2 is `ret 4` at its end). So the VM is *not* in ECX,
// and the argument has to come off exactly once, which is why the hook is written out by hand
// rather than as a C function: getting that wrong leaves the manager drawing whatever ECX happened
// to be, with the stack four bytes off, and the sprite stops being drawn at all.
static void* th8_draw_player_sprite = (void*)0x00462FF0;

static float th8_player_sprite_pos[2];

static void th8_player_sprite_nudge(void* vm) {
    float* pos = (float*)((char*)vm + 0x208);

    th8_player_sprite_pos[0] = pos[0];
    th8_player_sprite_pos[1] = pos[1];

    if (th8_extra_t != 0.0f) {
        pos[0] = th8_player_sprite_pos[0] + th8_player_step[0] * th8_extra_t;
        pos[1] = th8_player_sprite_pos[1] + th8_player_step[1] * th8_extra_t;
    }
}

static void th8_player_sprite_put_back(void* vm) {
    float* pos = (float*)((char*)vm + 0x208);

    pos[0] = th8_player_sprite_pos[0];
    pos[1] = th8_player_sprite_pos[1];
}

static __declspec(naked) void th8_player_sprite_hook() {
    __asm {
        push ebp
        mov ebp, esp
        push esi

        // The VM is the argument the caller pushed, not ECX
        mov esi, dword ptr [ebp + 8]

        push esi
        call th8_player_sprite_nudge
        add esp, 4

        // The game's own draw, called the way its caller calls it
        mov ecx, dword ptr ds:[018BDC90h]
        push esi
        call dword ptr [th8_draw_player_sprite]

        push esi
        call th8_player_sprite_put_back
        add esp, 4

        pop esi
        pop ebp
        ret 4
    }
}

// ---------------------------------------------------------------------------
// Frame interpolation, stage 1: the straight lasers
// ---------------------------------------------------------------------------
//
// The enemy lasers live in the same manager as the bullets, in 0x100 slots of 0x59C bytes at
// g_BulletManager+0x660938, with the flag that says a slot is in use at slot+0x584 (a 32 bit
// field). BulletManager::OnDraw (0x432B50) draws them first, before the bullets: it takes the slot,
// checks that flag (0x432BBB) and calls 0x433880 with the slot's angle (slot+0x554), which is what
// sets the sprites up from the laser's own geometry.
//
// That geometry is the laser's position (slot+0x548, three floats), its angle (slot+0x554), and the
// two ends it spans between: startOffset (slot+0x558) and endOffset (slot+0x55C), which the draw
// turns into the sprite's centre as startOffset + (endOffset - startOffset) / 2 (0x432BDE..0x432BF6,
// the 2.0f being the constant at 0x4B42EC). The update grows the laser by moving those: it adds
// speed times the frame's step to endOffset and then sets startOffset to endOffset - length
// (0x431BCF..0x431C1E, with speed at slot+0x56C and length at slot+0x560).
//
// The angle is not advanced by that update: the ECL opcode at 0x41BDA5 adds to it and normalises
// the result to +/-pi with FUN_0043EDB0 (0x43EDB0). Crossing that boundary therefore makes the
// raw per-frame difference almost 2pi even though the laser only turned a little. The angle step
// is therefore folded to the shortest arc, and the projected angle is folded back to +/-pi:
// theta = Wrap(theta1 + t * Wrap(theta2 - theta1)). Scripts can also set startOffset and endOffset
// in one step (0x41C3AD, 0x41C3F7); those are not motion and are not interpolated.
//
// None of those fields is written by the draw - it only reads them - so a laser is moved the way a
// bullet is: the frame's own movement of each of the four (position, angle and the two ends) is
// measured over a frame, the extras add their fraction of it before the pass and put the fields back
// after. That covers a laser that is still growing, one that has settled, one that turns and, if one
// moves, that as well, without the extras having to know which of them the laser is doing. A slot
// the game hands to a different laser between two samples is not projected across the jump, the same
// guard the shots have.

static const unsigned int th8_lasers_max = 0x100;
static const unsigned int th8_lasers_offset = 0x660938;    // from g_BulletManager
static const unsigned int th8_laser_size = 0x59C;
static const unsigned int th8_laser_in_use_offset = 0x584; // 32 bit, non-zero while in use
static const unsigned int th8_laser_position_offset = 0x548; // three floats
static const unsigned int th8_laser_angle_offset = 0x554;
static const unsigned int th8_laser_start_offset = 0x558;
static const unsigned int th8_laser_end_offset = 0x55C;

// What is measured and projected per laser: its position, its angle and the two ends
static const unsigned int th8_laser_fields = 6;

static const float th8_lasers_max_step = 48.0f;

static unsigned char* th8_laser_slot(unsigned int index) {
    // g_BulletManager (0x00F54E90) - the bullets below are in the same manager, so that is the
    // address named there as well
    return (unsigned char*)0x00F54E90 + th8_lasers_offset + (size_t)index * th8_laser_size;
}

static void th8_laser_read(unsigned int index, float* values) {
    const unsigned char* slot = th8_laser_slot(index);

    values[0] = *(const float*)(slot + th8_laser_position_offset);
    values[1] = *(const float*)(slot + th8_laser_position_offset + 4);
    values[2] = *(const float*)(slot + th8_laser_position_offset + 8);
    values[3] = *(const float*)(slot + th8_laser_angle_offset);
    values[4] = *(const float*)(slot + th8_laser_start_offset);
    values[5] = *(const float*)(slot + th8_laser_end_offset);
}

static float th8_lasers_last[th8_lasers_max][th8_laser_fields];
static float th8_lasers_step[th8_lasers_max][th8_laser_fields];
static bool th8_lasers_used_last[th8_lasers_max];

static unsigned int th8_lasers_in_play;

// Called once a frame, from the frame's first draw pass, next to th8_bullets_measure
static void th8_lasers_measure() {
    th8_lasers_in_play = 0;

    for (unsigned int i = 0; i < th8_lasers_max; ++i) {
        const bool in_use = *(const unsigned int*)(th8_laser_slot(i) + th8_laser_in_use_offset) != 0;
        float values[th8_laser_fields];
        th8_laser_read(i, values);

        const float step_x = values[0] - th8_lasers_last[i][0];
        const float step_y = values[1] - th8_lasers_last[i][1];
        const bool reused = (step_x * step_x + step_y * step_y) >
                            th8_lasers_max_step * th8_lasers_max_step;

        if (in_use)
            ++th8_lasers_in_play;

        for (unsigned int f = 0; f < th8_laser_fields; ++f) {
            float step = in_use && th8_lasers_used_last[i] && !reused
                             ? values[f] - th8_lasers_last[i][f]
                             : 0.0f;
            if (f == 3) {
                // The stored angle wraps at +/-pi; take the short way round the circle. This is
                // the inner Wrap of the shortest-arc formula; the outer Wrap is applied to the
                // result in th8_lasers_advance.
                while (step > 3.14159265f)
                    step -= 6.28318531f;
                while (step < -3.14159265f)
                    step += 6.28318531f;
            } else if (fabsf(step) > th8_lasers_max_step) {
                // Same for a scripted length/offset jump.
                step = 0.0f;
            }
            th8_lasers_step[i][f] = step;
            th8_lasers_last[i][f] = values[f];
        }
        th8_lasers_used_last[i] = in_use;
    }
}

static unsigned int th8_lasers_projected;
static unsigned int th8_lasers_moved[th8_lasers_max];
static float th8_lasers_saved[th8_lasers_max][th8_laser_fields];
static unsigned int th8_lasers_moved_count;

static void th8_lasers_advance(float t) {
    th8_lasers_moved_count = 0;

    for (unsigned int i = 0; i < th8_lasers_max; ++i) {
        unsigned char* slot = th8_laser_slot(i);
        if (*(const unsigned int*)(slot + th8_laser_in_use_offset) == 0)
            continue;

        float* fields[th8_laser_fields] = {
            (float*)(slot + th8_laser_position_offset),
            (float*)(slot + th8_laser_position_offset + 4),
            (float*)(slot + th8_laser_position_offset + 8),
            (float*)(slot + th8_laser_angle_offset),
            (float*)(slot + th8_laser_start_offset),
            (float*)(slot + th8_laser_end_offset),
        };

        for (unsigned int f = 0; f < th8_laser_fields; ++f) {
            th8_lasers_saved[th8_lasers_moved_count][f] = *fields[f];
            float value = *fields[f] + th8_lasers_step[i][f] * t;
            if (f == 3) {
                // Outer Wrap: theta = Wrap(theta1 + t * Wrap(theta2 - theta1)).
                while (value > 3.14159265f)
                    value -= 6.28318531f;
                while (value < -3.14159265f)
                    value += 6.28318531f;
            }
            *fields[f] = value;
        }
        th8_lasers_moved[th8_lasers_moved_count] = i;
        ++th8_lasers_moved_count;
    }

    th8_lasers_projected = th8_lasers_moved_count;
}

static void th8_lasers_undo() {
    for (unsigned int n = 0; n < th8_lasers_moved_count; ++n) {
        unsigned char* slot = th8_laser_slot(th8_lasers_moved[n]);
        float* fields[th8_laser_fields] = {
            (float*)(slot + th8_laser_position_offset),
            (float*)(slot + th8_laser_position_offset + 4),
            (float*)(slot + th8_laser_position_offset + 8),
            (float*)(slot + th8_laser_angle_offset),
            (float*)(slot + th8_laser_start_offset),
            (float*)(slot + th8_laser_end_offset),
        };

        for (unsigned int f = 0; f < th8_laser_fields; ++f)
            *fields[f] = th8_lasers_saved[n][f];
    }
    th8_lasers_moved_count = 0;
}

// ---------------------------------------------------------------------------
// Frame interpolation, stage 1: the familiars
// ---------------------------------------------------------------------------
//
// The familiars are four slots of 0x2F4 bytes at player+0x40C. Each slot carries its own update
// callback (+0x6F8 within the player) and draw callback (+0x6FC): the draw walks them and calls the
// callback each slot has (0x44D60E), the update does the same with the update one (0x44BD79). Both
// pointers are written at runtime, which is why the diagnostics name them - and the draw of one of
// them (0x44E9E0, for the callbacks the diagnostics printed) does what the player's own draw does:
// it copies the familiar's own position (slot+0x2A4, three floats) plus the screen shake into the
// position of the sprite VM the slot holds (slot+0x208, 0x44E9E5..0x44EA16), and then draws that.
//
// So a familiar moves the way a shot does: its own position is moved before the pass and put back
// after, and the step is measured over a frame - what moves a familiar is its update, which follows
// the player rather than reading a velocity. A slot only exists while its draw callback is set
// (the game's own check at 0x44D5E2): the solo characters have no familiars at all, which is what
// the diagnostics read as four zeroes.

static const unsigned int th8_familiars_max = 4;
static const unsigned int th8_familiar_size = 0x2F4;
static const unsigned int th8_familiar_pos_offset = 0x2A4;  // the draw copies this into the sprite

static float th8_familiars_last_pos[th8_familiars_max][3];
static float th8_familiars_step[th8_familiars_max][3];
static bool th8_familiars_used_last[th8_familiars_max];

static unsigned char* th8_familiar_slot(unsigned int index) {
    return th8_player + 0x40C + (size_t)index * th8_familiar_size;
}

static unsigned char** th8_familiar_draw_callback(unsigned int index) {
    return (unsigned char**)(th8_player + 0x6FC + (size_t)index * th8_familiar_size);
}

// Called once a frame, from the frame's first draw pass, next to th8_shots_measure. The same
// "nothing moves this far in a frame" guard as the shots have is used here: a slot can be given to
// a different familiar between two samples.
static const float th8_familiars_max_step = 48.0f;

static unsigned int th8_familiars_in_play;

static void th8_familiars_measure() {
    th8_familiars_in_play = 0;

    for (unsigned int i = 0; i < th8_familiars_max; ++i) {
        const bool in_use = *th8_familiar_draw_callback(i) != nullptr;
        const float* pos = (const float*)(th8_familiar_slot(i) + th8_familiar_pos_offset);

        const float step_x = pos[0] - th8_familiars_last_pos[i][0];
        const float step_y = pos[1] - th8_familiars_last_pos[i][1];
        const bool reused = (step_x * step_x + step_y * step_y) >
                            th8_familiars_max_step * th8_familiars_max_step;

        if (in_use)
            ++th8_familiars_in_play;

        for (int c = 0; c < 3; ++c) {
            th8_familiars_step[i][c] = in_use && th8_familiars_used_last[i] && !reused
                                           ? pos[c] - th8_familiars_last_pos[i][c]
                                           : 0.0f;
            th8_familiars_last_pos[i][c] = pos[c];
        }
        th8_familiars_used_last[i] = in_use;
    }
}

static unsigned int th8_familiars_projected;
static unsigned int th8_familiars_moved[th8_familiars_max];
static float th8_familiars_saved_pos[th8_familiars_max][3];
static unsigned int th8_familiars_moved_count;

static void th8_familiars_advance(float t) {
    th8_familiars_moved_count = 0;

    for (unsigned int i = 0; i < th8_familiars_max; ++i) {
        if (*th8_familiar_draw_callback(i) == nullptr)
            continue;

        float* pos = (float*)(th8_familiar_slot(i) + th8_familiar_pos_offset);
        for (int c = 0; c < 3; ++c) {
            th8_familiars_saved_pos[th8_familiars_moved_count][c] = pos[c];
            pos[c] = pos[c] + th8_familiars_step[i][c] * t;
        }
        th8_familiars_moved[th8_familiars_moved_count] = i;
        ++th8_familiars_moved_count;
    }

    th8_familiars_projected = th8_familiars_moved_count;
}

static void th8_familiars_undo() {
    for (unsigned int n = 0; n < th8_familiars_moved_count; ++n) {
        float* pos = (float*)(th8_familiar_slot(th8_familiars_moved[n]) + th8_familiar_pos_offset);
        for (int c = 0; c < 3; ++c)
            pos[c] = th8_familiars_saved_pos[n][c];
    }
    th8_familiars_moved_count = 0;
}

// ---------------------------------------------------------------------------
// Frame interpolation, stage 1: the player's own shots
// ---------------------------------------------------------------------------
//
// The player's shots are 0x80 slots of 0x484 bytes at player+0xBE838. The shot list's draw
// (0x4512F0, the first thing Player::OnDrawHighPrio runs) walks them, skips the ones whose state
// (slot+0x462, a 16 bit field) is not 1, copies each shot's own position (slot+0x2A4, three floats)
// plus the screen shake into the position of the sprite VM the slot holds (slot+0x208/+0x20C, at
// 0x451360..0x451387) and draws that VM through the slot's own callback (slot+0x478, 0x4513ED).
// Moving the shot's own position is therefore enough - the pass copies it into the sprite itself -
// and that needs no hook, whose calling convention would have to be matched to the byte (see the
// note above the player's sprite hook).
//
// A shot's own movement is its SHT script, run by the list's update through 0x40D3B0 (the script
// cursor is at slot+0x450), so there is no velocity field to read here either. Instead the frame's
// own movement is measured: every slot's position is sampled once a frame, in the game's own pass,
// and the difference to the frame before is what the extras project by. A slot that was not in use
// the frame before has just been spawned and gets no step for that frame.

static const unsigned int th8_shots_max = 0x80;
static const unsigned int th8_shots_offset = 0xBE838;      // from the player
static const unsigned int th8_shot_size = 0x484;
static const unsigned int th8_shot_in_use_offset = 0x462;  // 16 bit, 1 while the slot is in use
static const unsigned int th8_shot_pos_offset = 0x2A4;     // the pass copies this into the sprite

static float th8_shots_last_pos[th8_shots_max][3];
static float th8_shots_step[th8_shots_max][3];
static bool th8_shots_used_last[th8_shots_max];

// A shot that is done with its slot leaves it, and a new shot can be spawned into the same slot
// during the same frame, which the once-a-frame sampling here cannot see: the two positions are
// hundreds of pixels apart (the old one is up the screen where it died, the new one is at the
// player's), and projecting that jump draws the new shot somewhere below the player on the extras
// before it settles. Nothing a shot does in one frame comes anywhere near this far, so a step that
// big is thrown away instead of projected - that slot's shot is drawn where the frame's own pass put
// it for that one extra, and from the frame after on the step is the shot's own again.
static const float th8_shots_max_step = 48.0f;

static unsigned char* th8_shot_slot(unsigned int index) {
    return th8_player + th8_shots_offset + (size_t)index * th8_shot_size;
}

// Called once a frame, from the frame's first draw pass, next to th8_player_measure
static void th8_shots_measure() {
    for (unsigned int i = 0; i < th8_shots_max; ++i) {
        const unsigned char* slot = th8_shot_slot(i);
        const bool in_use = *(const short*)(slot + th8_shot_in_use_offset) == 1;
        const float* pos = (const float*)(slot + th8_shot_pos_offset);

        const float step_x = pos[0] - th8_shots_last_pos[i][0];
        const float step_y = pos[1] - th8_shots_last_pos[i][1];
        const bool reused = (step_x * step_x + step_y * step_y) >
                            th8_shots_max_step * th8_shots_max_step;

        for (int c = 0; c < 3; ++c) {
            // A slot that was not in use last frame, or one the game has reused, has just had a shot
            // spawned in it: its script has not run a step of its own yet, so there is nothing to
            // project by (see th8_shots_max_step)
            th8_shots_step[i][c] = in_use && th8_shots_used_last[i] && !reused
                                       ? pos[c] - th8_shots_last_pos[i][c]
                                       : 0.0f;
            th8_shots_last_pos[i][c] = pos[c];
        }
        th8_shots_used_last[i] = in_use;
    }
}

// What the last extra presentation moved, for the diagnostics
static unsigned int th8_shots_in_play;
static unsigned int th8_shots_projected;

static unsigned int th8_shots_moved[th8_shots_max];
static float th8_shots_saved_pos[th8_shots_max][3];
static unsigned int th8_shots_moved_count;

static void th8_shots_advance(float t) {
    th8_shots_moved_count = 0;
    th8_shots_in_play = 0;

    for (unsigned int i = 0; i < th8_shots_max; ++i) {
        unsigned char* slot = th8_shot_slot(i);
        if (*(short*)(slot + th8_shot_in_use_offset) != 1)
            continue;

        ++th8_shots_in_play;

        float* pos = (float*)(slot + th8_shot_pos_offset);
        for (int c = 0; c < 3; ++c) {
            th8_shots_saved_pos[th8_shots_moved_count][c] = pos[c];
            pos[c] = pos[c] + th8_shots_step[i][c] * t;
        }
        th8_shots_moved[th8_shots_moved_count] = i;
        ++th8_shots_moved_count;
    }

    th8_shots_projected = th8_shots_moved_count;
}

static void th8_shots_undo() {
    for (unsigned int n = 0; n < th8_shots_moved_count; ++n) {
        float* pos = (float*)(th8_shot_slot(th8_shots_moved[n]) + th8_shot_pos_offset);
        for (int c = 0; c < 3; ++c)
            pos[c] = th8_shots_saved_pos[n][c];
    }
    th8_shots_moved_count = 0;
}

// ---------------------------------------------------------------------------
// Frame interpolation, stage 1: the bullets
// ---------------------------------------------------------------------------
//
// The bullets the game draws live in g_BulletManager (0x00F54E90). The manager keeps six layer
// chains (layerListHeads at manager+0x6BA554, a bullet's nextInLayer at bullet+0xDC0) and its own
// draw walks them one layer after the other: BulletManager::OnDraw (0x432B50) takes each layer head
// (0x432EC3), calls BulletManager::DrawSingleBullet (0x432F20) down the chain (0x432EDE) for as
// long as there is a next one, and that draws the bullet's sprite at the bullet's own position
// (bullet+0xD44, plus the screen shake the game adds there at 0x432F9A/0x432FC6). Walking those six
// chains is therefore walking exactly what the frame's draw pass puts on screen, at the position
// the sprite is drawn at - not a pool or a guess at one.
//
// A bullet moves by its velocity added to its position once a frame, scaled by what it is doing at
// the time. The position update hands the two fields to the game's own Float3 helpers by address
// (`add ecx,0D50h` / `add ecx,0D44h`) and the add is Float3::operator+= (0x410A70):
//
//     state 1  BULLET_NORMAL           pos += velocity             (0x4314AE)
//     state 2  BULLET_SPAWNING_FAST    pos += velocity / 2.0f      (0x43179F, 0x40000000 pushed)
//     state 3  BULLET_SPAWNING_NORMAL  pos += velocity / 2.5f      (0x4318B1, 0x40200000 pushed)
//     state 4  BULLET_SPAWNING_SLOW    pos += velocity / 3.0f      (0x4319C2, 0x40400000 pushed)
//     state 5  BULLET_DESPAWN          pos += velocity / 2.0f      (0x431AC3, 0x40000000 pushed)
//
// The four spawning and despawning ones are the state switch of BulletManager::OnUpdate
// (0x4312DF..0x4312FF, its jump table at 0x432156), and each of them divides the velocity with
// Float3::operator/ (0x40C7D0) before adding it - that game's own tip for a bullet that is still
// moving into place, or already fading out. So an extra presentation puts the bullets where the
// frame's own step will put them t frames on: position += velocity * scale * t, drawn, and put
// straight back again - the game's own logic and its collision code still see the positions they
// last read. What it costs is that a bullet whose velocity changes within the step (an accelerating
// or turning one) is drawn a little off the path it will actually take, an error the fraction of a
// frame it is drawn a fraction of does not show.
//
// A bullet that is just flying has one more thing going on: while the game has time stopped, the
// update skips its step entirely (0x431490 reads g_GameManager.isTimeStopped at 0x160F534 and jumps
// over the add), so there is nothing to project for one. The states that are still moving into or
// out of the play field keep animating even then - their cases carry no such test.

// The manager's fields (see the comment above), and the pool size the manager's own chains can't
// hold more than
static const unsigned int th8_bullet_layers = 6;
static const unsigned int th8_bullet_layer_heads_offset = 0x6BA554;
static const unsigned int th8_bullet_count_offset = 0x6BA538; // the manager's own numActiveBullets
static const unsigned int th8_bullet_next_in_layer_offset = 0xDC0;
static const unsigned int th8_bullet_pos_offset = 0xD44;
static const unsigned int th8_bullet_velocity_offset = 0xD50;
static const unsigned int th8_bullet_state_offset = 0xDB8; // 16 bit, BULLET_NORMAL and up
static const unsigned int th8_bullets_max = 0x601; // MAX_ENEMY_BULLETS + 1
static unsigned char* th8_bullet_manager = (unsigned char*)0x00F54E90;

// g_GameManager.isTimeStopped (0x160F534, GameManager+0x2C)
static unsigned char* th8_time_stopped = (unsigned char*)0x0160F534;

// The fraction of a frame's step the update moves a bullet by in each of the states above
static float th8_bullet_step_scale(unsigned int state) {
    switch (state) {
        case 2: // BULLET_SPAWNING_FAST
        case 5: // BULLET_DESPAWN
            return 0.5f;
        case 3: // BULLET_SPAWNING_NORMAL
            return 0.4f; // 1 / 2.5
        case 4: // BULLET_SPAWNING_SLOW
            return 1.0f / 3.0f;
        default:
            // BULLET_NORMAL, and the only one of the five the update leaves alone while the game
            // has time stopped (see the comment above) - there is nothing to project for one then
            return *th8_time_stopped ? 0.0f : 1.0f;
    }
}

// What the last extra presentation moved, for the diagnostics (see th8_diag_dump)
static unsigned int th8_bullets_in_play;
static unsigned int th8_bullets_projected;
static unsigned int th8_bullets_managers_say;

// Where the bullets' positions were before they were moved, so they can be put back exactly (not
// worked out backwards - the positions have to come out bit for bit what the game left there)
static unsigned char* th8_projected_bullets[th8_bullets_max];
static float th8_projected_positions[th8_bullets_max][3];
static unsigned int th8_projected_count;

static void th8_bullets_advance(float t) {
    th8_projected_count = 0;

    for (unsigned int layer = 0; layer < th8_bullet_layers; ++layer) {
        unsigned char* bullet = *(unsigned char**)(th8_bullet_manager + th8_bullet_layer_heads_offset + layer * 4);
        while (bullet != nullptr) {
            if (th8_projected_count >= th8_bullets_max)
                break;

            const unsigned int state = *(const unsigned short*)(bullet + th8_bullet_state_offset);
            const float scale = th8_bullet_step_scale(state);
            float* pos = (float*)(bullet + th8_bullet_pos_offset);
            const float* velocity = (const float*)(bullet + th8_bullet_velocity_offset);

            th8_projected_bullets[th8_projected_count] = bullet;
            th8_projected_positions[th8_projected_count][0] = pos[0];
            th8_projected_positions[th8_projected_count][1] = pos[1];
            th8_projected_positions[th8_projected_count][2] = pos[2];
            ++th8_projected_count;

            pos[0] = pos[0] + velocity[0] * scale * t;
            pos[1] = pos[1] + velocity[1] * scale * t;
            pos[2] = pos[2] + velocity[2] * scale * t;

            bullet = *(unsigned char**)(bullet + th8_bullet_next_in_layer_offset);
        }
    }

    th8_bullets_in_play = th8_projected_count;
    th8_bullets_projected = th8_projected_count;
    th8_bullets_managers_say = *(unsigned int*)(th8_bullet_manager + th8_bullet_count_offset);
}

static void th8_bullets_undo() {
    for (unsigned int i = 0; i < th8_projected_count; ++i) {
        float* pos = (float*)(th8_projected_bullets[i] + th8_bullet_pos_offset);
        pos[0] = th8_projected_positions[i][0];
        pos[1] = th8_projected_positions[i][1];
        pos[2] = th8_projected_positions[i][2];
    }
    th8_projected_count = 0;
}

// The background's colour tint: g_Background is at 0x4E4030, its tint colour at +0x6468 and the
// "enabled" word at +0x646C. The draw pass applies the tint the ECL set and then resets the colour
// to 0x80808080 (0x4093be..0x4093dc), so an extra pass would apply the default grey instead of the
// illusion tint and the background would flash between the two.
static DWORD* const th8_background_tint = (DWORD*)0x004EA498;
static DWORD* const th8_background_tint_enabled = (DWORD*)0x004EA49C;
static DWORD th8_scene_tint;
static DWORD th8_scene_tint_enabled;
static DWORD th8_scene_tint_after;
static DWORD th8_scene_tint_enabled_after;

// Runs where the game's own draw pass starts: remembers what the scene is drawn with, watches for
// the tool's frame (its overlay is built by now, from the calc chain's return, and drawn on the
// return of the chain below), and then draws exactly the way the game always did.
static void __fastcall th8_render_hook(void* chain) {
    IDirect3DDevice8* device = th8_device();
    if (device != nullptr) {
        device->GetViewport(&th8_scene_viewport);
        th8_scene_transforms = SUCCEEDED(device->GetTransform(D3DTS_VIEW, &th8_scene_view)) &&
                               SUCCEEDED(device->GetTransform(D3DTS_PROJECTION, &th8_scene_projection));
    }

    // The screen shake this pass is about to draw with (see th8_scene_shake)
    if (*th8_anm_manager != nullptr) {
        th8_scene_shake[0] = *(float*)((char*)*th8_anm_manager + 0x1C);
        th8_scene_shake[1] = *(float*)((char*)*th8_anm_manager + 0x20);
    }

    // The tint this pass is about to apply (see th8_background_tint)
    th8_scene_tint = *th8_background_tint;
    th8_scene_tint_enabled = *th8_background_tint_enabled;

    ToolState::WatchFrameReady(th8_tool_frame_address, th8_tool_draw_address);

    // The frame's own pass (the extras run this hook as well, and their positions are the same ones
    // this frame already measured)
    if (!th8_drew_frame) {
        th8_player_measure();
        th8_shots_measure();
        th8_familiars_measure();
        th8_lasers_measure();
    }

    th8_drew_frame = true;
    th8_run_draw_chain(chain);
    th8_scene_tint_after = *th8_background_tint;
    th8_scene_tint_enabled_after = *th8_background_tint_enabled;
}

// One extra presentation of the frame: the render block the game runs, once more, so the state the
// game is in is drawn and presented again.
//
// t is where this presentation sits between the game's own one and the next one, i.e. how far the
// objects it draws are moved forward; t = 0 is the state the game's own presentation showed. The
// stage 1 code projects the objects forward by it.
static void th8_extra_presentation(float t) {
    IDirect3DDevice8* device = th8_device();
    if (device == nullptr)
        return;

    device->BeginScene();

    // Put back what the scene was drawn with (see th8_render_hook)
    if (th8_scene_transforms) {
        device->SetTransform(D3DTS_VIEW, &th8_scene_view);
        device->SetTransform(D3DTS_PROJECTION, &th8_scene_projection);
    }
    if (th8_scene_viewport.Width != 0)
        device->SetViewport(&th8_scene_viewport);

    // The shake the frame's own pass drew with (the HUD pass zeroed it before that pass ended - see
    // th8_scene_shake)
    if (*th8_anm_manager != nullptr) {
        *(float*)((char*)*th8_anm_manager + 0x1C) = th8_scene_shake[0];
        *(float*)((char*)*th8_anm_manager + 0x20) = th8_scene_shake[1];
    }

    // The block itself, in the order the game runs it (0x441FC8..0x442022)
    th8_clear_vertex_buffer(*th8_anm_manager);
    *th8_fog_state = 0xFF;
    th8_disable_fog(th8_supervisor);

    ToolState::ReadyToDraw();

    // Whatever was left bound (see th8_forget_anm_texture) is not what the manager thinks it is
    th8_forget_anm_texture();

    // Where the bullets are a fraction of a frame on from here; put back right after the pass drew
    // them (see the stage 1 comment above)
    if (Config::InterpolateBulletsAndLasers) {
        th8_bullets_advance(t);
    }
    if (Config::InterpolatePlayerShots) {
        th8_shots_advance(t);
    }
    if (Config::InterpolatePlayerBody) {
        th8_familiars_advance(t);
    }
    if (Config::InterpolateBulletsAndLasers) {
        th8_lasers_advance(t);
    }

    // The player's sprite, the familiars and the player's own shots are drawn from positions the
    // game's own code writes while drawing, so those are nudged where they are drawn instead - this
    // is what tells them how far along this presentation sits (see th8_player_sprite_hook)
    th8_extra_t = Config::InterpolatePlayerBody ? t : 0.0f;

    // The game counts its own frame rate off this pass (see the comment above), so its state is put
    // back around it: the extras draw the same frame rate text the game's own pass drew, and the
    // game's own accounting never sees them
    // The tint this pass applies (see th8_background_tint); the game's own pass reset the stored
    // colour to grey, so it has to be put back for every extra presentation.
    *th8_background_tint = th8_scene_tint;
    *th8_background_tint_enabled = th8_scene_tint_enabled;

    Th8FpsState fps;
    th8_fps_state_save(&fps);
    th8_run_draw_chain(th8_chain);
    th8_fps_state_restore(&fps);

    // Leave the state the game's own pass left, for the next game frame.
    *th8_background_tint = th8_scene_tint_after;
    *th8_background_tint_enabled = th8_scene_tint_enabled_after;

    th8_extra_t = 0.0f;

    th8_bullets_undo();
    th8_shots_undo();
    th8_familiars_undo();
    th8_lasers_undo();

    th8_flush_vertex_buffer(*th8_anm_manager);
    device->SetTexture(0, nullptr);
    device->EndScene();

    // Presented with the device's own Present, without the hooks other patches and tools put in
    // front of it (see d3d8_present_bypassing_hooks, the same thing th19 and th20 do): a tool's
    // present hook measures the input latency from the input poll to the next present, which only
    // means anything when there is one present per game frame - with the extras it would report a
    // fraction of a frame and take the extra presentations into the average. The patch's own
    // overlay is drawn from that path as well, so it stays on every presentation.
    d3d8_present_bypassing_hooks();
}

// Diagnostics: press U to write the presentation timing of the last frames to oilp_diag.txt (the
// same file name and ring buffer shape the th10-th20 diagnostics use)
static const unsigned int th8_diag_frames_max = 300;
static const unsigned int th8_diag_presents_max = 8;
struct Th8DiagFrame {
    LARGE_INTEGER base;                                 // the game's own present of the frame
    unsigned int presents;
    LARGE_INTEGER present_time[th8_diag_presents_max];  // when each presentation happened
};
static Th8DiagFrame th8_diag_frames[th8_diag_frames_max];
static unsigned int th8_diag_count;
static bool th8_diag_key_down;
static LARGE_INTEGER th8_diag_perf_freq;

// The game's own processing rate measurement over time (see Th8FpsState): what the two halves of
// the ratio are, what comes out of them, and the 500ms window its rate is counted with. The pair is
// reset when a stage is set up (GameManager::AddedCallback, GameManager.cpp:1145..1146), and its
// "not in a menu" gate is GameManager::flags.isActive - bit 2 of 0x164D0B4, set in that same
// callback and only cleared by the pause and retry menus (0x45231E tests it) - so the time a run
// spends on the results screen between stages is measured into the run as well. What that does
// depends on how many frames the game draws there, because the rate is the draw passes per half
// second: a window whose rate is under 54 adds less to the numerator while the 60 it adds to the
// denominator never changes, and the ratio the results screen and a saved replay read comes out
// lower (and the processing rate they show higher) for it.
struct Th8LagSample {
    unsigned int frame;         // the diagnostics' own frame counter when this was taken
    float numerator;
    float denominator;
    unsigned int flags;         // g_GameManager.flags (0x164D0B4)
    unsigned int window_count;  // 0x17CF4C0, the passes counted in the window that is open
    unsigned int window_age;    // ms since that window opened (0x17CF4B8 holds its start)
    unsigned int window_rate;   // that window's rate so far, passes per second
};
static Th8LagSample th8_lag_ring[64];
static unsigned int th8_lag_count;

static void th8_lag_sample() {
    Th8LagSample& sample = th8_lag_ring[th8_lag_count % 64];
    ++th8_lag_count;
    sample.frame = th8_diag_count;
    sample.numerator = *(float*)0x017CE8E8;
    sample.denominator = *(float*)0x017CE8EC;
    sample.flags = *(unsigned int*)0x0164D0B4;
    sample.window_count = *(unsigned int*)0x017CF4C0;
    sample.window_age = (unsigned int)timeGetTime() - *(unsigned int*)0x017CF4B8;
    sample.window_rate = sample.window_age > 0
                             ? (unsigned int)((float)sample.window_count * 1000.0f /
                                              (float)sample.window_age)
                             : 0;
}

static void th8_diag_dump() {
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

    unsigned int count = th8_diag_count < th8_diag_frames_max ? th8_diag_count : th8_diag_frames_max;
    unsigned int start = th8_diag_count < th8_diag_frames_max ? 0 : th8_diag_count % th8_diag_frames_max;

    fprintf(file, "oilp frame interpolation diagnostics\n");
    fprintf(file, "logic frame rate: %.2f, configured presentation rate: %.2f, display refresh: %uHz\n",
            Limiter::game_fps, Limiter::PresentRate(), Limiter::DisplayRefresh());
    fprintf(file, "configured game frame rate: %u, BltPrepareTime %u, sleep %d\n",
            Config::GameFPS, Config::BltPrepareTime, (int)Config::Sleep);
    fprintf(file, "frames recorded: %u\n", count);
    // The game's own frame rate accounting, from its own side (see Th8FpsState): the rate it read
    // out of the last 500ms window, the text it draws for it, and the lag it compensates for. Those
    // are counted off the draw passes the extras add, so reading what the game itself makes of them
    // is what says whether putting the state back works: steady 60 and "60.00fps" is right, and
    // about three times that is the extras being counted
    fprintf(file, "game's own frame rate: %d, its text: \"%.16s\", lag: %.4f\n",
            (int)*(short*)0x017CE8F0, (char*)0x017CF3B8, *(float*)0x017CE8E8);
    // The game's own "how much frame rate was lost" measurement (see Th8FpsState), which its score,
    // its rank and a replay's recorded processing rate all come out of: the ratio it works out of
    // the two is what those show, and its frame rate correction multiplier sits beside them
    fprintf(file, "game's own lag ratio: numerator %.4f, denominator %.4f, ratio %.4f, "
                  "frame rate correction %.4f\n",
            *(float*)0x017CE8E8, *(float*)0x017CE8EC,
            *(float*)0x017CE8EC != 0.0f
                ? *(float*)0x017CE8E8 / *(float*)0x017CE8EC
                : 0.0f,
            *(float*)0x017CE8E0);
    fprintf(file, "frameskip config: %u, loading (Supervisor::unk178): %u\n",
            (unsigned int)*(unsigned char*)0x017CE893, (unsigned int)*(DWORD*)0x017CE8D0);
    // The processing rate measurement over the last half minute (see Th8LagSample): the ratio and
    // what the window the rate is counted with looked like at each sample
    {
        unsigned int n = th8_lag_count < 64 ? th8_lag_count : 64;
        unsigned int ring_start = th8_lag_count < 64 ? 0 : th8_lag_count % 64;
        fprintf(file, "lag over time (every 30 frames, oldest first, %u samples):\n", n);
        for (unsigned int i = 0; i < n; ++i) {
            const Th8LagSample& sample = th8_lag_ring[(ring_start + i) % 64];
            fprintf(file,
                    "    frame %4u: numerator %9.2f denominator %9.2f ratio %7.4f flags %08x "
                    "window %3u passes in %4u ms (%u/s)\n",
                    sample.frame, sample.numerator, sample.denominator,
                    sample.denominator != 0.0f ? sample.numerator / sample.denominator : 0.0f,
                    sample.flags, sample.window_count, sample.window_age, sample.window_rate);
        }
    }
    // The bullets (see the stage 1 comment above): what the walk through the game's own layer
    // chains found, and what the manager's own counter says. A walk that stopped finding bullets
    // would say so right here. The counter reads a few higher than the chains hold, and that is the
    // update loop's own doing: it counts a bullet as active when it looks at it (0x4312CA), before
    // it notices that the bullet's despawn effect has finished, and those it turns off there
    // (0x432170 sets the bullet's state back to 0 and it is never linked into a layer) are still in
    // the count it leaves behind. The chains are the ones the frame's draw pass walks
    // (BulletManager::OnDraw, 0x432EA5), which is what the extras project.
    fprintf(file, "bullets: %u in play, %u projected forward (last presentation)\n",
            th8_bullets_in_play, th8_bullets_projected);
    fprintf(file, "bullets: the manager's own count says %u (the update's count, taken before it "
                  "turns the finished ones off)\n", th8_bullets_managers_say);
    // The lasers (see their stage 1 comment above): their slots are in the same manager as the
    // bullets, and the flag that says a slot is in use is the game's own
    fprintf(file, "lasers: %u in play, %u projected forward (last presentation)\n", th8_lasers_in_play,
            th8_lasers_projected);
    // The player (see the player's stage 1 comment above): how far the frame's own pass moved it,
    // which is what the extras project by
    fprintf(file, "player: %.2f px a frame (measured over the frames recorded)\n", th8_player_step_length);
    // The player's own shots (see their stage 1 comment above): how many slots are in use, and how
    // many of those the last presentation moved
    fprintf(file, "shots: %u in play, %u projected forward (last presentation)\n", th8_shots_in_play,
            th8_shots_projected);
    // The familiars (see their stage 1 comment above): the slots whose draw callback is set are the
    // ones the game itself draws, so that is what counts as one
    fprintf(file, "familiars: %u in play, %u projected forward (last presentation)\n",
            th8_familiars_in_play, th8_familiars_projected);
    // The familiars' callbacks are set at runtime, so the diagnostics name them: the code behind
    // them is what says where a familiar's own position lives (its slot is at player+0x40C+i*0x2F4,
    // its update callback at +0x6F8 and its draw callback at +0x6FC within the player)
    fprintf(file, "familiars: update %08x %08x %08x %08x\n",
            *(unsigned int*)(th8_player + 0x6F8), *(unsigned int*)(th8_player + 0x9EC),
            *(unsigned int*)(th8_player + 0xCE0), *(unsigned int*)(th8_player + 0xFD4));
    fprintf(file, "familiars: draw   %08x %08x %08x %08x\n",
            *(unsigned int*)(th8_player + 0x6FC), *(unsigned int*)(th8_player + 0x9F0),
            *(unsigned int*)(th8_player + 0xCE4), *(unsigned int*)(th8_player + 0xFD8));

    double presents = 0.0;
    double first = 0.0;
    double last = 0.0;
    for (unsigned int i = 0; i < count; ++i) {
        const Th8DiagFrame& frame = th8_diag_frames[(start + i) % th8_diag_frames_max];
        fprintf(file, "frame %4u: %u present(s)", i, frame.presents);
        for (unsigned int p = 0; p < frame.presents && p < th8_diag_presents_max; ++p)
            fprintf(file, " [%u] %.2fms", p,
                    (double)(frame.present_time[p].QuadPart - frame.base.QuadPart) * 1000.0 /
                        (double)th8_diag_perf_freq.QuadPart);
        fprintf(file, "\n");
        presents += frame.presents;
        if (i == 0)
            first = (double)frame.base.QuadPart;
        last = (double)frame.base.QuadPart;
    }
    if (last > first && count > 0)
        fprintf(file, "average: %.3f presents per frame, %.1f presentations per second\n",
                presents / (double)count, presents * (double)th8_diag_perf_freq.QuadPart / (last - first));

    fclose(file);
    printf("Wrote oilp_diag.txt\n");
}

// Presents the frame the game just drew, and then the extra presentations the config asked for.
static void __fastcall th8_present_hook(void* self) {
    if (th8_diag_perf_freq.QuadPart == 0)
        QueryPerformanceFrequency(&th8_diag_perf_freq);

    LARGE_INTEGER base;
    QueryPerformanceCounter(&base);

    // The game's own presentation of this frame, exactly as it always ran it. The capture of the
    // back buffer that the afterimage effects read happens inside it, before any of the extra
    // presentations below draw over it.
    th8_game_present(self);

    // The game's own work of this frame ends here as far as the overlay is concerned: everything
    // below is the patch presenting the same frame again
    Limiter::MarkFrameWorkEnd();

    // The tool has drawn its frame by now: the other half of watching for its frame state
    ToolState::WatchFrameDrawn();

    LARGE_INTEGER present_time[th8_diag_presents_max] = {};
    unsigned int presents = 1;
    present_time[0] = base;

    // The extras have nothing to draw in a frame the game didn't (see the frame skip note above),
    // and nothing to add while the presents are being thinned out - that is there to keep a frame
    // rate above the display's from being throttled by its presents, and extras would undo it
    if (th8_drew_frame && !Limiter::PresentingEveryNthFrame() && Limiter::ShouldPresent()) {
        double rate = Limiter::PresentRate();
        if (rate > 0.0) {
            static bool rate_printed = false;
            if (!rate_printed) {
                rate_printed = true;
                printf("Frame interpolation: %.1f presentations per second (%.2f per game frame)\n",
                       rate, rate / Limiter::game_fps);
            }
        }

        LARGE_INTEGER spacing;
        unsigned int extras = Limiter::ExtraPresentations(rate, spacing);

        // Where the game's own part of this frame started, so the extras can tell whether they
        // still fit before the next frame is due
        LARGE_INTEGER frame_start = Limiter::FrameStart();
        LARGE_INTEGER frame_wait = Limiter::FrameWait();
        LARGE_INTEGER game_work;
        game_work.QuadPart = base.QuadPart - frame_start.QuadPart;

        if (extras > 0 && spacing.QuadPart > 0) {
            // The presentations can't run past the end of the frame either: the limiter starts the
            // next frame one frame after this one started, and a frame that overruns it makes it
            // fall behind (and resync). An extra presentation costs about as much as the game's own
            // pass did, so the ones that wouldn't fit are dropped - the frame is then presented
            // fewer times, but never late.
            bool has_deadline = frame_start.QuadPart > 0 && game_work.QuadPart > 0;
            LARGE_INTEGER deadline;
            deadline.QuadPart = frame_start.QuadPart + frame_wait.QuadPart;

            for (unsigned int k = 1; k <= extras; ++k) {
                LARGE_INTEGER target;
                target.QuadPart = base.QuadPart + spacing.QuadPart * (LONGLONG)k;
                if (has_deadline && target.QuadPart + game_work.QuadPart > deadline.QuadPart)
                    continue;

                Limiter::WaitUntil(target);

                if (presents < th8_diag_presents_max)
                    QueryPerformanceCounter(&present_time[presents]);
                ++presents;

                // Where this presentation sits between the game's own one and the next one: the
                // presentations of one game frame are spread evenly over it, so the k-th of
                // extras + 1 of them draws the state k/(extras + 1) frames along
                th8_extra_presentation((float)k / (float)(extras + 1));
            }
        }
    }

    th8_drew_frame = false;

    // Record the frame for the diagnostics
    {
        Th8DiagFrame& frame = th8_diag_frames[th8_diag_count % th8_diag_frames_max];
        frame.base = base;
        frame.presents = presents;
        for (unsigned int p = 0; p < th8_diag_presents_max; ++p)
            frame.present_time[p] = p < presents ? present_time[p] : present_time[0];
        ++th8_diag_count;

        // Every half second, so the samples cover the last ~30 seconds of play: where the ratio
        // falls (or stops falling) is what says which part of a run is being measured into it
        if (th8_diag_count % 30 == 0)
            th8_lag_sample();
    }

    // U dumps the diagnostics
    if (GetAsyncKeyState('U') & 0x8000) {
        if (!th8_diag_key_down) {
            th8_diag_key_down = true;
            th8_diag_dump();
        }
    } else {
        th8_diag_key_down = false;
    }
}

// Takes over the draw chain call (to watch what the scene is drawn with and where the tool's frame
// is) and the present call that follows it
static void th8_install_interpolation() {
    {
        // call 0x43CB60, Chain::RunDrawChain (0x441FEC); the draw itself runs exactly the way it
        // always did
        BYTE* site = (BYTE*)0x00441FEC;
        if (site[0] == 0xE8 && *(DWORD*)(site + 1) == 0x0043CB60 - (0x00441FEC + 5)) {
            patch_call(0x00441FEC, th8_render_hook);
        } else {
            printf("th08 draw chain call at 0x441FEC doesn't look like a known one, skipping!\n");
        }
    }
    {
        // call 0x442060, GameWindow::Present (0x44203D)
        BYTE* site = (BYTE*)0x0044203D;
        if (site[0] == 0xE8 && *(DWORD*)(site + 1) == 0x00442060 - (0x0044203D + 5)) {
            patch_call(0x0044203D, th8_present_hook);
        } else {
            printf("th08 present call at 0x44203D doesn't look like a known one, skipping!\n");
        }
    }
    {
        // The rate the frames are actually presented at, next to the render time line in the bottom
        // left corner - the same readout th10 to th20 have. Which of the overlay's readouts are shown
        // is Config::ShowOverlay's business rather than the interpolation's, and is set once in
        // Limiter::Initialize; the presentation counting itself lives in the D3D8 hook, which sees
        // every present.
    }
    {
        // call 0x462FF0, the draw of the player's sprite VM (0x44D5B9), taken over so the extras can
        // nudge the position the game has just written into that VM (see the player's stage 1
        // comment above)
        BYTE* site = (BYTE*)0x0044D5B9;
        if (site[0] == 0xE8 && *(DWORD*)(site + 1) == 0x00462FF0 - (0x0044D5B9 + 5)) {
            patch_call(0x0044D5B9, th8_player_sprite_hook);
        } else {
            printf("th08 player sprite draw at 0x44D5B9 doesn't look like a known one, skipping!\n");
        }
    }
    printf("Frame interpolation: th08 extra presentations installed (setting %d)\n", Config::Interpolation);
}

FPSTarget th8_replay_callback() {
    if (CEngine::Instance->state == 2 && ((CGame::Instance->flags >> 3) & 1) != 0) {
        auto input = get_input();
        if (input & InputState::Focus)
            return FPSTarget::ReplaySlow;
        else if (input & (InputState::Skip | InputState::Shoot))
            return FPSTarget::ReplaySkip;
    }
    return FPSTarget::Game;
}
