// Touhou 6: Embodiment of Scarlet Devil v1.02h

#include <stdio.h>
#include <math.h>
#include <string.h>
#include "patch_util.h"
#include "limiter.h"
#include "d3d8_hook.h"
#include "overlay.h"
#include "tool_state.h"
#include "touhou6.h"
#include "config.h"

using namespace Touhou6;

CEngine* CEngine::Instance = (CEngine*)0x006C6D18;
CWindowManager* CWindowManager::Instance = (CWindowManager*)0x006C6BD4;
CChainManager* CChainManager::Instance = (CChainManager*)0x0069D918;
CGame* CGame::Instance = (CGame*)0x0069BCA0;

// The frame interpolation below draws a frame from the calculation chain hook when it is on - see
// the stage 0 comment at the end of this file. These are the parts of it the hook itself needs.
static bool th6_interpolating;
static bool th6_drew_frame;
static void th6_begin_game_draw();
static void th6_draw_pass();

// HACK: This should actually be taking in a parameter and using that in place of CChainManager::Instance
// However, if I did that, the edi register gets corrupted by CChainManager::UpdateCalcChain
// ...and that causes a crash because "this" is saved in that register in the generated assembly for this function
// Doing it this way seems to work fine. More investigation required...
int __fastcall th6_update_calc_chain_hook() {
	auto engine = CEngine::Instance;
    // The device only exists after the game initialises D3D, so install lazily (idempotent).
    D3D8Hook::Install(engine->d3d8_device);
    Limiter::Tick();

    auto ret = CChainManager__UpdateCalcChain(CChainManager::Instance);
    if (ret != 0 && ret != -1 && engine->config.frameskip <= (int)CWindowManager::Instance->frames_skipped) {
        // The frame's own draw (see the frame interpolation comment below, which is where the
        // block lives now)
        if (th6_interpolating)
            th6_begin_game_draw();
        th6_draw_pass();
        th6_drew_frame = true;
    }

    return ret;
}

int __fastcall th6_window_update_hook() {
    // Skipping the window update while the window isn't active saves CPU, but the window
    // update is also what drives rendering, so with AlwaysBlt we have to keep calling it.
    if (!CWindowManager::Instance->activated && !Config::AlwaysBlt) {
        Sleep(16);
        return 0;
    }
    return CWindowManager__Update(CWindowManager::Instance);
}

// ---------------------------------------------------------------------------
// The frame rate the game measures for itself (vpatch's CalcFPS)
//
// Touhou 6 works its own frame rate out in DrawFpsCounter (0x423F3F): every frame it adds
// 1 + frameskipConfig to the counter at 0x6C6D08, and once half a second has gone by (taking the
// time from timeGetTime) it divides the counter by that time and clears it - that reading is what
// tells the game how much of the half second it lost, which it accumulates into 0x6C6ECC over
// 0x6C6ED0 (Supervisor::unk1b4 / unk1b8). Those two are the "processing rate" the results screen
// shows (ResultScreen.cpp:1670) and the value a replay records (ReplayManager.cpp:400); a window
// that a load or a scene change lands in reads as a slow one, and the run then carries that as lost
// frame rate for the rest of it.
//
// vpatch's CalcFPS=1 replaces that measurement: it takes over 0x423F45 (the first instruction of
// the counter, th06's own vpatch continues at 0x423FE7 with the frame rate its stub 60003DC0
// computed left in the game's fps variable). This does the same with the frame rate the limiter
// measured for itself (see Limiter::CountGameFrame): the game's counter and window are skipped, the
// game is handed that reading, and it carries on where it would have - which leaves its own
// accounting (and everything that reads it) with a run that never fell behind.
// ---------------------------------------------------------------------------
static void* th6_fps_continue(void* frame) {
    // The frame pointer this is handed is the game's own; a hook that gets the calling convention
    // wrong hands over something else entirely, which used to mean writing through it. Writing only
    // to something that can be a stack address keeps that mistake from becoming memory corruption.
    if ((size_t)frame < 0x10000)
        return (void*)0x00423FE7;
    // The counter the game adds to is left where its own code would have left it: cleared once the
    // window it belongs to is over
    *(unsigned int*)0x006C6D08 = 0;
    *(float*)((char*)frame - 0x0C) = Limiter::MeasuredFps();
    // Where vpatch continues: just before the game formats and draws the frame rate text, with the
    // per half second weighting (0x424001 on) left to run as the game wrote it
    return (void*)0x00423FE7;
}

static __declspec(naked) void th6_fps_hook() {
    // The game's own frame pointer, pushed the way the game's own calls pass their arguments (it
    // is what the fps variable is written through, see th6_fps_continue)
    __asm { push ebp }
    __asm { call th6_fps_continue }
    __asm { add esp, 4 }
    __asm { jmp eax }
}

// Takes over the two present calls of GameWindow::Render (defined below, after the comment that
// explains what a frame is made of)
static void th6_install_interpolation();

void th6_install_patches() {
	{
		// Skip the original frame limiter
		BYTE patch[] = { 0xE9, 0x9E, 0x00, 0x00, 0x00 };
		patch_bytes(0x004208ED, patch, sizeof(patch));
	}
	{
		// Skip original draw chain update logic
		BYTE patch[] = { 0xE9, 0xEE, 0x00 };
		patch_bytes(0x0042071A, patch, sizeof(patch));
	}
	{
		// Hook calc chain update
		// Shaves off 1 frame of input lag by calling calc chains BEFORE drawing
		patch_call(0x00420853, th6_update_calc_chain_hook);
	}
    {
        // Hook window updating
        // Avoid eating up CPU time while minimized
        patch_call(0x004204FF, th6_window_update_hook);
    }
    if (Config::FixInputGlitching) {
        // Fix input glitching
        BYTE patch[] = { 0x00, 0x00, 0x00, 0x00, 0x74 };
        patch_bytes(0x0041DC58, patch, sizeof(patch));
    }
    if (Config::AlwaysBlt) {
        // GameWindow::Render returns early when [this+8] (isAppActive) is 0, which stops
        // rendering entirely once the window loses focus and freezes the picture.
        // Turn that conditional jump into an unconditional one (same idea as vpatch's AlwaysBlt).
        BYTE patch[] = { 0xEB, 0x07 }; // jne 004206F9 -> jmp 004206F9
        patch_bytes(0x004206F0, patch, sizeof(patch));
    }
    if (Config::CalcFPS) {
        // Replace the frame rate the game measures for itself (see the comment above)
        BYTE* site = (BYTE*)0x00423F45;
        if (site[0] == 0xA1 && *(DWORD*)(site + 1) == 0x006C6D10) {
            patch_jmp(0x00423F45, th6_fps_hook);
        } else {
            printf("th06 fps measurement at 0x423F45 doesn't look like a known one, skipping!\n");
        }
    }
    if (Config::Interpolation != 0 || Config::InterpolationFPS != 0)
        th6_install_interpolation();
}

FPSTarget th6_replay_callback() {
    if (CEngine::Instance->state == 2 && CGame::Instance->is_replay) {
        auto input = get_input();
        if (input & InputState::Focus)
            return FPSTarget::ReplaySlow;
        else if (input & (InputState::Skip | InputState::Shoot))
            return FPSTarget::ReplaySkip;
    }
    return FPSTarget::Game;
}

// ---------------------------------------------------------------------------
// Frame interpolation, stage 0: extra presentations
// ---------------------------------------------------------------------------
//
// A frame of this game, the way the patches above leave it:
//
//     Limiter::Tick()                    the patch's wait for the next frame ends here, at the top
//                                        of the calculation chain hook (0x420853)
//     Chain::RunCalcChain (0x41CA10)     the input is read at its head, by Supervisor_OnUpdate
//                                        (its chain priority is 0, see ChainPriorities.hpp)
//     [the frame's draw - see below]
//     GameWindow_Present (0x420B50)      the device's Present, and then
//                                        AnmManager::TakeScreenshotIfRequested (0x421A40), which
//                                        reads the back buffer back for the snapshot key
//
// The game's own draw of a frame is the block at 0x420720..0x420807 of GameWindow::Render
// (0x4206E0): a 640x480 viewport and a Clear when the config asks for one (0x420720 tests
// g_Supervisor.cfg.opts at 0x6C6E60, bits 3 and 4, which is ShouldForceBackbufferClear), BeginScene,
// Chain::RunDrawChain (0x41CAD0), EndScene, SetTexture(0, NULL). The patch jumps over the game's own
// copy of it (the branch at 0x42071A is made unconditional and lands at 0x42080D) and runs the same
// block from the calculation chain hook instead, which is what puts the calculation ahead of the
// drawing and takes the frame of input lag out (see the comment on that hook). The limiter paces the
// frame from the top of that hook, so what is left of a frame after the game's own present is where
// the extra presentations belong - the same shape th07, th08, th19 and th20 use.
//
// An extra presentation is therefore the whole calculation chain hook with its presentation taken
// out: wait until that presentation's moment, draw the block again, present. Nothing in the
// calculation chain runs for one, so the game's logic, its timers and its random numbers are
// untouched. The present calls at 0x420A01 and 0x420A0B (the two exits of GameWindow::Render) are
// taken over to place them.
//
// Three things have to be kept out of the extras:
//
//   - The game counts its own frame rate off the draw pass. DrawFpsCounter (0x423F3F, the draw chain
//     callback Supervisor_OnDraw at CHAIN_PRIO_DRAW_SUPERVISOR = 14) adds one plus the frame skip
//     setting to g_NumFramesSinceLastTime (0x6C6D08, 0x423F71..0x423F82) every pass, and once half a
//     second has gone by works the frame rate out of it (0x423F8A..0x423FE0) - that reading goes into
//     the "%.02ffps" text in the bottom right corner (sprintf at 0x423FF9 into g_FpsCounterBuffer,
//     0x6C6C08) and, while the game is in a menu, into how much frame rate the run lost
//     (g_Supervisor.unk1b4 at 0x6C6ECC and unk1b8 at 0x6C6ED0, 0x424001..0x4240B5, both of them
//     written from every pass with CalcFPS on). Their ratio is what the results screen shows
//     (ResultScreen.cpp:1670) and what a replay records (ReplayManager.cpp:400), so run once per
//     extra presentation and the run would carry frame rate it never lost. The state that function
//     works in is put back around every extra pass (th6_fps_state below), which leaves the game's
//     accounting exactly where its own pass left it and makes the extras draw the same frame rate
//     text that pass drew.
//   - The game's own frame skip (cfg.frameskipConfig, 0x6C6E4B, against CWindowManager's curFrame at
//     +0x10) can leave a frame with no draw pass at all: it then presents the picture it already had.
//     The extras have nothing of their own to draw in that frame, so they are left out of it (see
//     th6_drew_frame) and the game's own present simply runs at its own rate.
//   - Whatever the frame's own pass was started with has to be put back before an extra draws: the
//     pass writes state the next pass reads. See th6_begin_game_draw for what that state is here.
//
// On text, this game's draw passes are self-contained the way th07's and th08's are: the string list
// is drawn and then emptied inside the same pass (AsciiManager_OnDrawMenus, at draw chain priority
// 15, runs DrawStrings and then numStrings = 0), and what appends to it (the HUD at priority 11, the
// frame rate counter at 14) runs earlier in that same pass. So an extra pass draws the same text the
// game's own pass drew instead of stacking a copy of it on top.

// The device the frame is drawn with and presented through. The game reads it from here itself
// (0x420774, 0x420B5C): g_Supervisor.d3dDevice, i.e. CEngine::Instance + 8
static IDirect3DDevice8* th6_device() {
    return *(IDirect3DDevice8**)0x006C6D20;
}

// The game's own frame, the block described above, as one function: the calculation chain hook runs it
// for the frame's own draw, and every extra presentation runs it again. The draw chain takes no lock
// of its own, so it can be run from inside the frame the same way the game runs it.
//
// The Clear's colour is g_Stage.skyFog.color: the game reads it from 0x00487B60 in this very block
// (0x42078A) and again when it clears the depth buffer (0x41B857). g_Stage is 0x00487B10 and its
// skyFog starts at +0x48 (Stage.hpp), so the colour is at +0x50. g_Supervisor.viewport is at 0x6C6DE0
// (g_Supervisor is 0x6C6D18, viewport at +0xC8), which is the CEngine::viewport named here.
static void th6_draw_pass() {
    auto engine = CEngine::Instance;

    if (((engine->config.engine_flags >> 4) & 1) | ((engine->config.engine_flags >> 3) & 1)) {
        D3DVIEWPORT8 viewport;
        viewport.X = 0;
        viewport.Y = 0;
        viewport.Width = 640;
        viewport.Height = 480;
        viewport.MinZ = 0.0;
        viewport.MaxZ = 1.0;
        engine->d3d8_device->SetViewport(&viewport);
        engine->d3d8_device->Clear(0, 0, 3, *(D3DCOLOR*)0x00487B60, 1.0, 0);
        engine->d3d8_device->SetViewport(&engine->viewport);
    }
    engine->d3d8_device->BeginScene();
    CChainManager__UpdateDrawChain(CChainManager::Instance);
    engine->d3d8_device->EndScene();
    engine->d3d8_device->SetTexture(0, 0);
}

// g_Supervisor.viewport. The play field is drawn inside a viewport of its own, and it is written from
// both sides of a frame:
//
//   - GameManager_OnUpdate (calc chain priority 4) sets it to the play field out of
//     g_GameManager.gameRegionScreenPos / gameRegionSize (GameManager.cpp:116), builds the stage's
//     camera out of its size (GameManager::SetupCamera, GameManager.cpp:511: centre = size/2, aspect
//     = width/height, distance = halfHeight / tan(fov/2); it sets D3DTS_VIEW and D3DTS_PROJECTION with
//     it) and sets the device's viewport to it.
//   - The HUD, the full screen effects, the menus and the results screen set it back to the whole
//     640x480 screen while they draw (Gui.cpp:1115, ScreenEffect.cpp:31 and :133, AsciiManager.cpp:217,
//     ResultScreen.cpp:1744), and Stage_OnDrawLowPrio sets its depth range for the stage background
//     (Stage.cpp:263).
//
// So the value an extra pass finds there is the one the frame's own pass *ended* with, and the extras
// have to start from the one that pass started with, the same way th07 does. The device's own viewport
// and the view / projection matrices are put back with it (the draw callbacks write those as well:
// Stage_OnDrawLowPrio rebuilds the stage background's camera from the viewport at Stage.cpp:265, and
// the menus set their own transforms before drawing).
//
// The view and projection matrices exist twice, and both have to be put back: g_Supervisor.viewMatrix
// (0x6C6D60) and g_Supervisor.projectionMatrix (0x6C6DA0) are what the stage is *projected* with -
// Stage::RenderObjects culls every object by projecting it through them (Stage.cpp:512-546) and places
// the billboard quads (autoRotate == 2) by projecting their corners with them and drawing the result
// (Stage.cpp:625-640) - while the device holds its own copy of the two. Both are set together by
// GameManager::SetupCamera (GameManager.cpp:507) in the calculation chain, and then *changed* during
// the pass by the stage background's own camera (GameManager::SetupCameraStageBackground, called from
// Stage_OnDrawLowPrio at Stage.cpp:265, GameManager.cpp:534). So at the end of a pass they hold the
// background's camera, at the start of the next one the calculation chain has put the play field's
// back - and an extra pass that only restored the device's would cull and place the stage with a
// camera the frame's own pass never used.
struct Th6SceneState {
    D3DVIEWPORT8 game_viewport;   // g_Supervisor.viewport, 0x6C6DE0
    D3DVIEWPORT8 device_viewport; // as the device has it when the frame's own draw starts
    D3DMATRIX view;               // g_Supervisor.viewMatrix, 0x6C6D60
    D3DMATRIX projection;         // g_Supervisor.projectionMatrix, 0x6C6DA0
    D3DMATRIX device_view;
    D3DMATRIX device_projection;
    bool captured;
    bool has_transforms;
};
static Th6SceneState th6_scene;

// The depth buffer. The calculation chain clears it once a frame - GameManager_OnUpdate sets the play
// field viewport, builds the camera, sets the device's viewport and then clears the depth buffer out
// of it (GameManager.cpp:125-126, Clear(0, NULL, D3DCLEAR_ZBUFFER, g_Stage.skyFog.color, 1.0f, 0)) -
// so the frame's own draw starts with an empty one. A pass that runs after the frame's own pass does
// not: it is depth tested against the values that pass left behind, which are the depths of
// everything it drew, including what it drew last. The HUD is drawn last and its quads sit in front
// of the stage background and of circles drawn behind an enemy's sprite - so those, which the frame's
// own pass draws first and without anything in front of them, would fail the depth test in the extras
// and simply not be drawn there. That is a picture that changes between the presentations of one
// frame (the frame rate the extras present at), which shows as flashing. Clearing the depth buffer
// with the viewport the game clears it with puts the extra pass back in the depth state the frame's
// own pass started from; it is done outside BeginScene, the same way the game does it.
static void th6_clear_depth_buffer(IDirect3DDevice8* device) {
    device->Clear(0, nullptr, D3DCLEAR_ZBUFFER, *(D3DCOLOR*)0x00487B60, 1.0f, 0);
}

// The AnmManager's caches of what it has already told the device to do (AnmManager::current*). An
// extra presentation draws through the same manager, and a cache that still says "this is already
// set" makes the pass skip the corresponding device call - but the pass it repeats ended by setting
// the device's texture to NULL (the block's last step) and, with ShowOverlay on, by the patch's
// overlay binding its own font texture and states, which a D3D8 state block does not put back. So the
// caches do not describe the device any more, and they are all forgotten here so that the pass sets
// what it draws with, the way the game's own pass does when it starts a frame.
//
// The one that matters most in this game is the sprite cache. The texture bind sits *inside* the
// "the sprite changed" branch here (AnmManager::DrawInner, AnmManager.cpp:538-554 = 0x43288E..0x4328E8:
// `cmp currentSprite, vm->sprite` and only then `cmp currentTexture, textures[sourceFileIndex]`), so a
// sprite whose pointer is the same as the one that was drawn last skips the bind as well - and since
// the game's own pass ends with SetTexture(0, NULL), that leaves those sprites drawn with no texture
// at all (a white quad, the vertex colour) until some draw uses a different sprite. That is what the
// first test showed as bullets and lasers coming out washed out/overexposed in places, and it made the
// extra presentations differ from the frame's own one. th07 and th08 only forget the texture because
// their texture check is not nested inside the sprite one; here both have to go.
//
// The values the caches are set to are the ones Supervisor_OnDraw itself resets them to at the start
// of every draw pass (0x4237A8 sprite = NULL, 0x4237BA texture = NULL, 0x4237CC vertex shader = 0xFF,
// 0x4237DE blend mode = 0xFF, and the colour op / z write disable the same way), i.e. the game's own
// "nothing is set yet" values. currentTextureFactor is deliberately left alone: this pass does not
// keep a record of what the game sets it to either.
static const unsigned int th6_anm_texture_offset = 0x210B8;
static const unsigned int th6_anm_blend_mode_offset = 0x210BC;
static const unsigned int th6_anm_color_op_offset = 0x210BD;
static const unsigned int th6_anm_vertex_shader_offset = 0x210BE;
static const unsigned int th6_anm_z_write_offset = 0x210BF;
static const unsigned int th6_anm_sprite_offset = 0x210C0;

static void th6_forget_anm_cache() {
    unsigned char* manager = *(unsigned char**)0x006D4588;
    if (manager == nullptr)
        return;

    *(void**)(manager + th6_anm_sprite_offset) = nullptr;
    *(void**)(manager + th6_anm_texture_offset) = nullptr;
    manager[th6_anm_blend_mode_offset] = 0xFF;
    manager[th6_anm_color_op_offset] = 0xFF;
    manager[th6_anm_vertex_shader_offset] = 0xFF;
    manager[th6_anm_z_write_offset] = 0xFF;
}

// ---------------------------------------------------------------------------
// Frame interpolation, stage 1: the bullets
// ---------------------------------------------------------------------------
//
// The enemy bullets live in g_BulletManager (0x005A5FF8), in the 640 slots of BulletManager::bullets
// at manager+0x5600 (MAX_ENEMY_BULLETS, sizeof(Bullet) == 0x5C4; the manager adds 0x5600 to reach the
// array at 0x4149DD and 0x4149DD+... for each slot, 0x414A26). Unlike th07 and th08 there are no
// layer chains here: the manager's own draw (BulletManager_OnDraw, 0x416500) walks all 640 slots four
// times over - by the bullet's height and sprite, for the draw order: bulletHeight > 16, == 16 with
// the ring/ball sprite, == 16 with the others, and == 8 (BulletManager.cpp:1215-1269) - and draws
// every slot whose state (bullet+0x5BE, a 16 bit field, read at 0x416B5B) is not
// BULLET_STATE_INACTIVE. That is the set the extras project.
//
// Bullet::Draw in the later games is DrawBullet here (0x416B50): it picks the sprite by state (the
// spawn effect sprites for the spawning and despawning states, the bullet's own sprite otherwise),
// copies the bullet's own position into it (bullet+0x560 to the sprite's pos at +0x90, three floats,
// 0x416BBB..0x416BE5) and draws it. So, as in th07, a bullet needs no hook - its own position is
// moved before the extra pass and put back the moment the pass drew it, bit for bit rather than
// worked out backwards, which leaves the game's logic, its collision code and its random numbers with
// the positions they last read.
//
// How far a bullet moves in a frame is its velocity (bullet+0x56C, three floats) scaled by what it is
// doing at the time, times the frame's effective frame rate multiplier (this is the one thing in this
// game whose step is scaled by it):
//
//     BULLET_STATE_FIRED           pos += velocity                    (0x414xxx, BulletManager.cpp:876)
//     BULLET_STATE_SPAWNING_FAST   pos += velocity / 2.0f              (BulletManager.cpp:672)
//     BULLET_STATE_SPAWNING_NORMAL pos += velocity / 2.5f              (BulletManager.cpp:681)
//     BULLET_STATE_SPAWNING_SLOW   pos += velocity / 3.0f              (BulletManager.cpp:690)
//     BULLET_STATE_DESPAWNING      pos += velocity / 2.0f              (BulletManager.cpp:935)
//
// and each of those is multiplied by g_Supervisor.effectiveFramerateMultiplier (0x6C6EC0). Reading
// the velocity is what th07's bullets do and it is exact here as well: the update runs the bullet's
// velocity effects *before* the step (BulletManager.cpp:700-876: EX_SPEEDUP, EX_ACCELERATION,
// EX_VELOCITY, the angle changes and the bounces all rewrite the velocity first), so what the field
// holds when the update returns is the vector the step used - and a bullet that is not moving is
// left where it is.
//
// While the game has time stopped the whole update returns at its top - 0x4149E5 reads
// g_GameManager.isTimeStopped (0x69BCCC, GameManager+0x2C) and jumps straight to CONTINUE - so
// nothing moves, and neither do the extras: the bullets stay exactly where the game froze them.

static const unsigned int th6_bullets_max = 640;             // MAX_ENEMY_BULLETS
static const unsigned int th6_bullet_size = 0x5C4;           // sizeof(Bullet)
static const unsigned int th6_bullet_array_offset = 0x5600;  // BulletManager::bullets
static const unsigned int th6_bullet_pos_offset = 0x560;     // three floats
static const unsigned int th6_bullet_velocity_offset = 0x56C;
static const unsigned int th6_bullet_state_offset = 0x5BE;   // 16 bit
static const unsigned int th6_bullet_count_offset = 0xF5C04; // BulletManager::bulletCount
static unsigned char* th6_bullet_manager = (unsigned char*)0x005A5FF8; // g_BulletManager

// g_GameManager.isTimeStopped (0x69BCCC, GameManager+0x2C)
static unsigned char* th6_time_stopped = (unsigned char*)0x0069BCCC;

// g_Supervisor.effectiveFramerateMultiplier (0x6C6EC0, Supervisor+0x1A8): the factor the update
// scales every bullet's step by
static float* th6_effective_framerate = (float*)0x006C6EC0;

static unsigned char* th6_bullet_slot(unsigned int index) {
    return th6_bullet_manager + th6_bullet_array_offset + (size_t)index * th6_bullet_size;
}

// The fraction of a frame's step the update moves a bullet by in each of the states above
static float th6_bullet_step_scale(unsigned int state) {
    switch (state) {
        case 2: // BULLET_STATE_SPAWNING_FAST
        case 5: // BULLET_STATE_DESPAWNING
            return 0.5f;
        case 3: // BULLET_STATE_SPAWNING_NORMAL
            return 0.4f; // 1 / 2.5
        case 4: // BULLET_STATE_SPAWNING_SLOW
            return 1.0f / 3.0f;
        default: // BULLET_STATE_FIRED
            return 1.0f;
    }
}

// What the last extra presentation found and moved, for the diagnostics
static unsigned int th6_bullets_in_play;
static unsigned int th6_bullets_projected;
static unsigned int th6_bullets_managers_say;

// Where the bullets' positions were before they were moved, so they can be put back exactly
static unsigned char* th6_projected_bullets[th6_bullets_max];
static float th6_projected_positions[th6_bullets_max][3];
static unsigned int th6_projected_count;

static void th6_bullets_advance(float t) {
    th6_projected_count = 0;

    // Nothing moves while the game has time stopped (see the comment above), so neither do the extras
    if (*th6_time_stopped)
        return;

    const float multiplier = *th6_effective_framerate;

    for (unsigned int i = 0; i < th6_bullets_max; ++i) {
        unsigned char* bullet = th6_bullet_slot(i);

        const unsigned int state = *(const unsigned short*)(bullet + th6_bullet_state_offset);
        if (state == 0) // BULLET_STATE_INACTIVE
            continue;

        const float scale = th6_bullet_step_scale(state) * multiplier;
        float* pos = (float*)(bullet + th6_bullet_pos_offset);
        const float* velocity = (const float*)(bullet + th6_bullet_velocity_offset);

        th6_projected_bullets[th6_projected_count] = bullet;
        th6_projected_positions[th6_projected_count][0] = pos[0];
        th6_projected_positions[th6_projected_count][1] = pos[1];
        th6_projected_positions[th6_projected_count][2] = pos[2];
        ++th6_projected_count;

        pos[0] = pos[0] + velocity[0] * scale * t;
        pos[1] = pos[1] + velocity[1] * scale * t;
        pos[2] = pos[2] + velocity[2] * scale * t;
    }

    th6_bullets_in_play = th6_projected_count;
    th6_bullets_projected = th6_projected_count;
    th6_bullets_managers_say = *(unsigned int*)(th6_bullet_manager + th6_bullet_count_offset);
}

static void th6_bullets_undo() {
    for (unsigned int i = 0; i < th6_projected_count; ++i) {
        float* pos = (float*)(th6_projected_bullets[i] + th6_bullet_pos_offset);
        pos[0] = th6_projected_positions[i][0];
        pos[1] = th6_projected_positions[i][1];
        pos[2] = th6_projected_positions[i][2];
    }
    th6_projected_count = 0;
}

// ---------------------------------------------------------------------------
// Frame interpolation, stage 1: the player, its two orbs and its own shots
// ---------------------------------------------------------------------------
//
// All three are drawn from positions the draw copies into the sprite it is about to draw, so none of
// them needs a hook - what the extras move is what the draw reads:
//
//   - The player. Player_OnDrawHighPrio (0x429930) writes the sprite's position from the player's own
//     positionCenter (Player+0x440, two floats; the corner of the play field, which GameManager keeps
//     at 0x69D6DC/0x69D6E0, is added at 0x42996D..0x429994) and draws it with
//     AnmManager::DrawNoRotation, then does the same for the orbs out of orbsPosition[0] and [1]
//     (Player+0x4A0 and +0x4AC, 0x4299F9..0x429AEB, drawn with AnmManager::Draw, z = 0.491f). The
//     player moves with the input, the shot type's speed (and a bomb multiplier) and is clamped to
//     the movement area, and a death or a stage transition puts it somewhere else outright, so what
//     one frame moved it by is measured rather than read out of a velocity - the same thing th07 and
//     th08 do for theirs.
//   - The orbs are this game's familiars: two of them, drawn while the player has power 8 or more
//     (Player.cpp:704: below that the game hides them, orbState = ORB_HIDDEN, and from 8 on it runs
//     ORB_UNFOCUSED / FOCUSING / FOCUSED / UNFOCUSING with focusMovementTimer) and placed from the
//     player's position with an offset that swings between 24 and 8 pixels out and 32 up as the focus
//     key is held. The shots of the shot types that have a spawnPositionIdx fire from them
//     (Player.cpp:75). Their step is not a velocity either: their offset moves with that little state
//     machine, so it is measured the same way the player's is.
//   - The player's own shots. DrawBullets (0x429B00) walks the 80 slots of Player::bullets
//     (Player+0xA28, sizeof(PlayerBullet) == 0x158) and draws the ones whose bulletState
//     (bullet+0x14E, 16 bit) is PLAYER_BULLET_STATE_FIRED, out of the slot's own sprite (the AnmVm at
//     the start of the slot, drawn with AnmManager::Draw2); Player_OnDrawLowPrio (0x429B90) draws the
//     ones that have collided (state 2) the same way, as their explosion, out of the same sprite
//     (DrawBulletExplosions, 0x429BB0). What is drawn is therefore the sprite's own position
//     (slot+0x90), not the bullet's logical one (slot+0x110) - and that is what the extras move, so
//     nothing of the game's own state is written at all. The update keeps the two in step
//     (UpdatePlayerBullets, Player.cpp:933: sprite.pos.x = position.x += velocity.x * the frame rate
//     multiplier), but how far a shot actually went is the difference between two frames' drawn
//     positions, measured the way the player's is, which covers the shot types whose update places
//     them outright as well.
//
// The step of each is the difference to the position the frame before had, sampled once a frame where
// the game's own draw starts (th6_begin_game_draw) - after the calculation chain has run, which is
// where the positions are the ones the frame is drawn with. Nothing of this is written anywhere but
// those positions, and they are put back the moment the pass drew, so the game's logic, its
// collisions and its random numbers see exactly what they last read.
//
// A shot's drawn position that jumped further in one frame than anything can move is not projected
// across that jump: a slot that was free is given to a shot that was just spawned, which the
// once-a-frame sampling cannot see, and the same guard th07 and th08 use (48 pixels, against the
// fastest of these shots' ten or so pixels a frame) keeps that from being drawn as a fraction of the
// jump.

static const unsigned int th6_player_position_offset = 0x440;      // Float3, two of them used
static const unsigned int th6_player_orb_positions_offset = 0x4A0; // D3DXVECTOR3[2]
static const unsigned int th6_player_bullets_offset = 0xA28;       // PlayerBullet[80]
static const unsigned int th6_player_bullet_size = 0x158;
static const unsigned int th6_player_bullet_sprite_pos_offset = 0x90; // the drawn position
static const unsigned int th6_player_bullet_state_offset = 0x14E;     // 16 bit
static const unsigned int th6_player_bullets_max = 80;
static unsigned char* th6_player = (unsigned char*)0x006CA628;     // g_Player
static const unsigned int th6_orbs = 2;

// A shot cannot move anywhere near this far in a frame, so a step this big is a slot that was given
// to something else
static const float th6_shots_max_step = 48.0f;

// The player is moved by the input and then clamped to its movement area, which is a few pixels a
// frame at most (the character data's movement speeds are single digits, and a bomb multiplies them
// by at most two). A death, a respawn or a stage transition puts it somewhere else outright - Player
// ::Die moves it back to the respawn point at the bottom of the play field - and the orbs are placed
// from the player's position, so they jump with it. A frame that moved either of them further than
// anything can move is not projected across that jump, the same guard the shots have: the extras then
// draw them exactly where the frame's own pass drew them (at the respawn point) instead of a
// fraction of the way down to it, which showed as the player flashing at a spot just below the
// respawn point. th07 and th08 do not need this - their player is teleported the same way, but the
// extra presentations of the frame a death lands in did not read as a flash there.
static const float th6_player_max_step = 48.0f;

// Where one measured position was, and how far it moved over the frame (the draws set the z of
// everything here themselves, so only x and y are projected)
struct Th6Step {
    float last_x;
    float last_y;
    float step_x;
    float step_y;
    bool known;
};

static void th6_step(Th6Step* state, const float* pos, float limit) {
    if (state->known) {
        state->step_x = pos[0] - state->last_x;
        state->step_y = pos[1] - state->last_y;

        if (limit > 0.0f && state->step_x * state->step_x + state->step_y * state->step_y > limit * limit)
            state->step_x = state->step_y = 0.0f;
    } else {
        state->step_x = state->step_y = 0.0f;
        state->known = true;
    }

    state->last_x = pos[0];
    state->last_y = pos[1];
}

static Th6Step th6_player_step;
static Th6Step th6_orb_steps[th6_orbs];
static Th6Step th6_shot_steps[th6_player_bullets_max];

// What the last extra presentation found, for the diagnostics (see th6_diag_dump)
static unsigned int th6_shots_in_play;
static unsigned int th6_shots_flying;
static unsigned int th6_shots_exploding;
static unsigned int th6_shots_projected;
static float th6_player_step_length;
static float th6_orb_step_lengths[th6_orbs];

// Called once a frame, from the frame's own draw
static void th6_player_measure() {
    const float* pos = (const float*)(th6_player + th6_player_position_offset);
    th6_step(&th6_player_step, pos, th6_player_max_step);
    th6_player_step_length = (float)sqrt((double)(th6_player_step.step_x * th6_player_step.step_x +
                                                  th6_player_step.step_y * th6_player_step.step_y));

    for (unsigned int orb = 0; orb < th6_orbs; ++orb) {
        th6_step(&th6_orb_steps[orb],
                 (const float*)(th6_player + th6_player_orb_positions_offset + orb * 12),
                 th6_player_max_step);
        th6_orb_step_lengths[orb] =
            (float)sqrt((double)(th6_orb_steps[orb].step_x * th6_orb_steps[orb].step_x +
                                 th6_orb_steps[orb].step_y * th6_orb_steps[orb].step_y));
    }
}

static void th6_shots_measure() {
    th6_shots_in_play = 0;
    th6_shots_flying = 0;
    th6_shots_exploding = 0;

    for (unsigned int i = 0; i < th6_player_bullets_max; ++i) {
        unsigned char* shot = th6_player + th6_player_bullets_offset + (size_t)i * th6_player_bullet_size;
        const int state = *(const short*)(shot + th6_player_bullet_state_offset);

        // A slot that is not in use is forgotten rather than measured across what was there before it:
        // the shot that gets it next has not moved at all yet (see the comment above)
        if (state == 0) {
            th6_shot_steps[i].known = false;
            continue;
        }

        ++th6_shots_in_play;
        if (state == 1)
            ++th6_shots_flying;
        else
            ++th6_shots_exploding;

        th6_step(&th6_shot_steps[i], (const float*)(shot + th6_player_bullet_sprite_pos_offset),
                 th6_shots_max_step);
    }
}

// What the last extra presentation moved, so it can be put back
static float th6_saved_player_pos[2];
static float th6_saved_orb_pos[th6_orbs][2];
static unsigned char* th6_moved_shots[th6_player_bullets_max];
static float th6_saved_shot_pos[th6_player_bullets_max][2];
static unsigned int th6_moved_shot_count;

static void th6_player_advance(float t) {
    float* pos = (float*)(th6_player + th6_player_position_offset);
    th6_saved_player_pos[0] = pos[0];
    th6_saved_player_pos[1] = pos[1];
    pos[0] = pos[0] + th6_player_step.step_x * t;
    pos[1] = pos[1] + th6_player_step.step_y * t;

    for (unsigned int orb = 0; orb < th6_orbs; ++orb) {
        float* orb_pos = (float*)(th6_player + th6_player_orb_positions_offset + orb * 12);
        th6_saved_orb_pos[orb][0] = orb_pos[0];
        th6_saved_orb_pos[orb][1] = orb_pos[1];
        orb_pos[0] = orb_pos[0] + th6_orb_steps[orb].step_x * t;
        orb_pos[1] = orb_pos[1] + th6_orb_steps[orb].step_y * t;
    }
}

static void th6_player_undo() {
    float* pos = (float*)(th6_player + th6_player_position_offset);
    pos[0] = th6_saved_player_pos[0];
    pos[1] = th6_saved_player_pos[1];

    for (unsigned int orb = 0; orb < th6_orbs; ++orb) {
        float* orb_pos = (float*)(th6_player + th6_player_orb_positions_offset + orb * 12);
        orb_pos[0] = th6_saved_orb_pos[orb][0];
        orb_pos[1] = th6_saved_orb_pos[orb][1];
    }
}

static void th6_shots_advance(float t) {
    th6_moved_shot_count = 0;

    for (unsigned int i = 0; i < th6_player_bullets_max; ++i) {
        unsigned char* shot = th6_player + th6_player_bullets_offset + (size_t)i * th6_player_bullet_size;
        if (*(const short*)(shot + th6_player_bullet_state_offset) == 0)
            continue;

        float* pos = (float*)(shot + th6_player_bullet_sprite_pos_offset);
        th6_saved_shot_pos[th6_moved_shot_count][0] = pos[0];
        th6_saved_shot_pos[th6_moved_shot_count][1] = pos[1];
        pos[0] = pos[0] + th6_shot_steps[i].step_x * t;
        pos[1] = pos[1] + th6_shot_steps[i].step_y * t;
        th6_moved_shots[th6_moved_shot_count] = shot;
        ++th6_moved_shot_count;
    }

    th6_shots_projected = th6_moved_shot_count;
}

static void th6_shots_undo() {
    for (unsigned int n = 0; n < th6_moved_shot_count; ++n) {
        float* pos = (float*)(th6_moved_shots[n] + th6_player_bullet_sprite_pos_offset);
        pos[0] = th6_saved_shot_pos[n][0];
        pos[1] = th6_saved_shot_pos[n][1];
    }
    th6_moved_shot_count = 0;
}

// ---------------------------------------------------------------------------
// Frame interpolation, stage 1: the straight lasers
// ---------------------------------------------------------------------------
//
// The enemy lasers live in the same manager as the bullets, in the 64 slots of BulletManager::lasers
// at g_BulletManager+0xEC000 = 0x6921F8 (MAX_ENEMY_LASERS, sizeof(Laser) == 0x270; the update reaches
// them with 0x415DF7 `add eax, 0EC000h` and walks them with 0x415E14 `add edx, 270h`, the draw with
// 0x416521 and 0x41653E), with the flag that says a slot is in use at slot+0x258 (a 32 bit field,
// tested at 0x415E2A and 0x416554).
//
// BulletManager_OnDraw draws them before the bullets: it takes the slot, checks that flag, and works
// the sprite's position out of the laser's own geometry - the angle (slot+0x22C, fed to fsincos at
// 0x41657A) and the two ends it spans between, startOffset (slot+0x230) and endOffset (slot+0x234):
// the sprite is centred at startOffset + (endOffset - startOffset) / 2 (0x416586..0x4165A7, the 2.0f
// being the constant at 0x46A44C) away from the laser's own position (slot+0x220, x and y - the draw
// sets the sprite's z to 0 itself, so the laser's own z is not used) and the play field's corner is
// not added to it. When startOffset < 16 or the laser has stopped moving that pass it draws the
// glow at its base as well (BulletManager.cpp:1180-1215), which the draw works out of the same two
// offsets and the width, so it follows whatever those fields say.
//
// The update grows the laser by moving those two (BulletManager_OnUpdate, 0x415E38..0x415EB8:
// `endOffset += speed * the frame rate multiplier`, the speed being slot+0x240, and then
// `startOffset = endOffset - startLength`, the length being slot+0x238, clamped to 0), and works the
// length and width out into the sprite's own scale (slot+0x1C is scaleX = width / the sprite's width
// in pixels, slot+0x18 is scaleY = the length / its height, 0x415F21..0x415F5A); the offsets the
// draw reads are only ever read by the draw. So a laser is moved the way a bullet is, except that
// what moves is measured rather than read out of a speed: the frame's own movement of each of the
// fields the draw uses - the position's x and y, the angle, the two offsets and the sprite's two
// scales - is measured over a frame, and the extras add their fraction of it before the pass and put
// the fields back after it. That covers a laser that is still growing, one that has settled, one that
// is turning and one that is despawning (which shrinks it in the width) without the extras having to
// know which of those it is doing.
//
// A slot the game hands to a different laser between two samples is not projected across the jump,
// the same guard the bullets and the shots have. While the game has time stopped the laser update
// never runs either (the same return at the top of BulletManager_OnUpdate that freezes the bullets),
// so the measured step of a frozen laser is zero and the extras leave it where it is.

static const unsigned int th6_lasers_max = 64;                // MAX_ENEMY_LASERS
static const unsigned int th6_laser_size = 0x270;             // sizeof(Laser)
static const unsigned int th6_laser_array_offset = 0xEC000;   // from g_BulletManager
static const unsigned int th6_laser_pos_offset = 0x220;       // three floats, x and y used
static const unsigned int th6_laser_angle_offset = 0x22C;
static const unsigned int th6_laser_start_offset_offset = 0x230;
static const unsigned int th6_laser_end_offset_offset = 0x234;
static const unsigned int th6_laser_in_use_offset = 0x258;    // 32 bit, non-zero while in use
static const unsigned int th6_laser_scale_y_offset = 0x18;    // the sprite's length
static const unsigned int th6_laser_scale_x_offset = 0x1C;    // the sprite's width

// What is measured and projected per laser: its position, its angle, the two ends, and the two scales
// the update worked the length and the width out into
static const unsigned int th6_laser_fields = 7;

static const float th6_lasers_max_step = 48.0f;

static unsigned char* th6_laser_slot(unsigned int index) {
    // g_BulletManager (0x005A5FF8) - the bullets are in the same manager, so that is the address
    // named there as well
    return th6_bullet_manager + th6_laser_array_offset + (size_t)index * th6_laser_size;
}

static void th6_laser_read(unsigned int index, float* values) {
    const unsigned char* slot = th6_laser_slot(index);

    values[0] = *(const float*)(slot + th6_laser_pos_offset);
    values[1] = *(const float*)(slot + th6_laser_pos_offset + 4);
    values[2] = *(const float*)(slot + th6_laser_angle_offset);
    values[3] = *(const float*)(slot + th6_laser_start_offset_offset);
    values[4] = *(const float*)(slot + th6_laser_end_offset_offset);
    values[5] = *(const float*)(slot + th6_laser_scale_y_offset);
    values[6] = *(const float*)(slot + th6_laser_scale_x_offset);
}

static float th6_lasers_last[th6_lasers_max][th6_laser_fields];
static float th6_lasers_step[th6_lasers_max][th6_laser_fields];
static bool th6_lasers_used_last[th6_lasers_max];

static unsigned int th6_lasers_in_play;

// Called once a frame, from the frame's own draw, next to th6_shots_measure
static void th6_lasers_measure() {
    th6_lasers_in_play = 0;

    for (unsigned int i = 0; i < th6_lasers_max; ++i) {
        const bool in_use = *(const unsigned int*)(th6_laser_slot(i) + th6_laser_in_use_offset) != 0;
        float values[th6_laser_fields];
        th6_laser_read(i, values);

        const float step_x = values[0] - th6_lasers_last[i][0];
        const float step_y = values[1] - th6_lasers_last[i][1];
        const bool reused = (step_x * step_x + step_y * step_y) > th6_lasers_max_step * th6_lasers_max_step;

        if (in_use)
            ++th6_lasers_in_play;

        for (unsigned int f = 0; f < th6_laser_fields; ++f) {
            th6_lasers_step[i][f] = in_use && th6_lasers_used_last[i] && !reused
                                        ? values[f] - th6_lasers_last[i][f]
                                        : 0.0f;
            th6_lasers_last[i][f] = values[f];
        }
        th6_lasers_used_last[i] = in_use;
    }
}

static unsigned int th6_lasers_projected;
static unsigned int th6_lasers_moved[th6_lasers_max];
static float th6_lasers_saved[th6_lasers_max][th6_laser_fields];
static unsigned int th6_lasers_moved_count;

static void th6_lasers_advance(float t) {
    th6_lasers_moved_count = 0;

    for (unsigned int i = 0; i < th6_lasers_max; ++i) {
        unsigned char* slot = th6_laser_slot(i);
        if (*(const unsigned int*)(slot + th6_laser_in_use_offset) == 0)
            continue;

        float* fields[th6_laser_fields] = {
            (float*)(slot + th6_laser_pos_offset),
            (float*)(slot + th6_laser_pos_offset + 4),
            (float*)(slot + th6_laser_angle_offset),
            (float*)(slot + th6_laser_start_offset_offset),
            (float*)(slot + th6_laser_end_offset_offset),
            (float*)(slot + th6_laser_scale_y_offset),
            (float*)(slot + th6_laser_scale_x_offset),
        };

        for (unsigned int f = 0; f < th6_laser_fields; ++f) {
            th6_lasers_saved[th6_lasers_moved_count][f] = *fields[f];
            *fields[f] = *fields[f] + th6_lasers_step[i][f] * t;
        }
        th6_lasers_moved[th6_lasers_moved_count] = i;
        ++th6_lasers_moved_count;
    }

    th6_lasers_projected = th6_lasers_moved_count;
}

static void th6_lasers_undo() {
    for (unsigned int n = 0; n < th6_lasers_moved_count; ++n) {
        unsigned char* slot = th6_laser_slot(th6_lasers_moved[n]);
        float* fields[th6_laser_fields] = {
            (float*)(slot + th6_laser_pos_offset),
            (float*)(slot + th6_laser_pos_offset + 4),
            (float*)(slot + th6_laser_angle_offset),
            (float*)(slot + th6_laser_start_offset_offset),
            (float*)(slot + th6_laser_end_offset_offset),
            (float*)(slot + th6_laser_scale_y_offset),
            (float*)(slot + th6_laser_scale_x_offset),
        };

        for (unsigned int f = 0; f < th6_laser_fields; ++f)
            *fields[f] = th6_lasers_saved[n][f];
    }
    th6_lasers_moved_count = 0;
}

// The state DrawFpsCounter (0x423F3F) works in. It is called from the draw chain, so every extra
// presentation runs it once more, and this is what puts the game's accounting back the way its own
// pass left it:
//
//     0x6C6D08  g_NumFramesSinceLastTime, the count of passes in the current half second
//               (0x423F71, 0x423F78 and 0x423F82 add to it, 0x423FE0 clears it)
//     0x6C6D0C  g_LastTime, where that half second started (0x423F63 at startup, 0x423FDB when a
//               window closes)
//     0x6C6C08  g_FpsCounterBuffer, the "%.02ffps" text the count is drawn as (sprintf at 0x423FF9,
//               handed to g_AsciiManager.AddString at 0x4240E9 unless the game is in an ending)
//     0x6C6ECC  Supervisor::unk1b4, and 0x6C6ED0 unk1b8, how much frame rate the run lost; the two of
//               them are accumulated while the game is in a menu (0x424001..0x4240B5) and their ratio
//               is what the results screen shows (ResultScreen.cpp:1670) and a replay records
//               (ReplayManager.cpp:400)
//     0x6C6D10  the "the static local above has been initialised" flag, which 0x423F58 writes once on
//               the game's first call
//
// That list is every absolute address the function writes (0x423F3F to 0x4240F0), plus the text buffer
// its sprintf writes into. They are saved and put back as one block, which is what keeps the game's
// own reading of its frame rate at 60 and its lost frame rate at zero no matter how many times a frame
// is presented.
struct Th6FpsState {
    unsigned int frames_since_last_time; // 0x6C6D08
    unsigned int last_time;              // 0x6C6D0C
    unsigned int last_time_guard;        // 0x6C6D10
    char fps_text[256];                  // 0x6C6C08
    float rate_accumulator;              // g_Supervisor.unk1b4, 0x6C6ECC
    float rate_denominator;              // g_Supervisor.unk1b8, 0x6C6ED0
};

static void th6_fps_state_save(Th6FpsState* state) {
    state->frames_since_last_time = *(unsigned int*)0x006C6D08;
    state->last_time = *(unsigned int*)0x006C6D0C;
    state->last_time_guard = *(unsigned int*)0x006C6D10;
    memcpy(state->fps_text, (void*)0x006C6C08, sizeof(state->fps_text));
    state->rate_accumulator = *(float*)0x006C6ECC;
    state->rate_denominator = *(float*)0x006C6ED0;
}

static void th6_fps_state_restore(const Th6FpsState* state) {
    *(unsigned int*)0x006C6D08 = state->frames_since_last_time;
    *(unsigned int*)0x006C6D0C = state->last_time;
    *(unsigned int*)0x006C6D10 = state->last_time_guard;
    memcpy((void*)0x006C6C08, state->fps_text, sizeof(state->fps_text));
    *(float*)0x006C6ECC = state->rate_accumulator;
    *(float*)0x006C6ED0 = state->rate_denominator;
}

// Where the user's tool builds its frame and where it draws it (see tool_state.h). thprac's th06
// module hooks the return of Chain::RunCalcChain (0x41CAAC) with its "th06_update" hook to build its
// overlay frame and the return of Chain::RunDrawChain (0x41CB6D) with its "th06_render" hook to draw
// it (thprac_th06.cpp, EHOOK_DY(th06_update, 0x41caac) and EHOOK_DY(th06_render, 0x41cb6d)); both are
// plain `ret`s, so the defaults are right for them.
static unsigned char* th6_tool_frame_address = (unsigned char*)0x0041CAAC;
static unsigned char* th6_tool_draw_address = (unsigned char*)0x0041CB6D;

// Runs where the game's own draw starts: remembers what the frame is drawn with, and watches for the
// tool's frame (its overlay is built by now, from the calculation chain's return, and drawn on the
// return of the draw chain inside the block below).
static void th6_begin_game_draw() {
    IDirect3DDevice8* device = th6_device();

    th6_scene.captured = device != nullptr;
    th6_scene.has_transforms = false;
    if (device != nullptr) {
        device->GetViewport(&th6_scene.device_viewport);
        th6_scene.has_transforms =
            SUCCEEDED(device->GetTransform(D3DTS_VIEW, &th6_scene.device_view)) &&
            SUCCEEDED(device->GetTransform(D3DTS_PROJECTION, &th6_scene.device_projection));
    }
    th6_scene.game_viewport = *(D3DVIEWPORT8*)0x006C6DE0;
    th6_scene.view = *(D3DMATRIX*)0x006C6D60;
    th6_scene.projection = *(D3DMATRIX*)0x006C6DA0;

    // The frame's own movement of the player, its two orbs and its own shots (see the stage 1
    // comment above): sampled once a frame, where the calculation chain has already run. (With the
    // game's own frame skip turned on a frame can run the calculation chain and the draw more than
    // once; the sample is taken at the first of those, the same way th07 and th08 take it, which
    // leaves the extras projecting a frame's worth of movement the frame before the last one in that
    // case.)
    if (!th6_drew_frame) {
        th6_player_measure();
        th6_shots_measure();
        th6_lasers_measure();
    }

    ToolState::WatchFrameReady(th6_tool_frame_address, th6_tool_draw_address);
}

// One extra presentation of the frame: the block the game draws a frame with, once more, so the state
// the game is in is drawn and presented again.
//
// t is where this presentation sits between the game's own one and the next one, i.e. how far the
// objects it draws are moved forward; t = 0 is the state the game's own presentation showed. The
// stage 1 code below projects the objects forward by it.
static void th6_extra_presentation(float t) {
    IDirect3DDevice8* device = th6_device();
    if (device == nullptr)
        return;

    // Put back what the frame's own draw was started with (see th6_begin_game_draw)
    if (th6_scene.captured) {
        *(D3DVIEWPORT8*)0x006C6DE0 = th6_scene.game_viewport;
        device->SetViewport(&th6_scene.device_viewport);
        // Both copies of the camera, the game's own (what the stage is projected with) and the
        // device's
        *(D3DMATRIX*)0x006C6D60 = th6_scene.view;
        *(D3DMATRIX*)0x006C6DA0 = th6_scene.projection;
        if (th6_scene.has_transforms) {
            device->SetTransform(D3DTS_VIEW, &th6_scene.device_view);
            device->SetTransform(D3DTS_PROJECTION, &th6_scene.device_projection);
        }
    }

    // The manager's caches do not describe the device any more (see th6_forget_anm_cache)
    th6_forget_anm_cache();

    // The frame's own pass drew against an empty depth buffer; this pass has to as well (the viewport
    // above is the one the game clears it with)
    th6_clear_depth_buffer(device);

    // The tool's overlay is drawn from the return of the draw chain inside this block, so it has to
    // know its frame is ready to be drawn again
    ToolState::ReadyToDraw();

    // Where the objects are a fraction of a frame on from here; put back right after the pass drew
    // them (see the stage 1 comments above)
    if (Config::InterpolateBulletsAndLasers) {
        th6_bullets_advance(t);
    }
    if (Config::InterpolatePlayerBody) {
        th6_player_advance(t);
    }
    if (Config::InterpolatePlayerShots) {
        th6_shots_advance(t);
    }
    if (Config::InterpolateBulletsAndLasers) {
        th6_lasers_advance(t);
    }

    // The game counts its own frame rate off this pass (see the comment above), so its state is put
    // back around it
    Th6FpsState fps;
    th6_fps_state_save(&fps);
    th6_draw_pass();
    th6_fps_state_restore(&fps);

    th6_bullets_undo();
    th6_player_undo();
    th6_shots_undo();
    th6_lasers_undo();

    // Presented with the device's own Present, without the hooks other patches and tools put in front
    // of it (see d3d8_present_bypassing_hooks, the same thing th07, th08, th19 and th20 do): a tool's
    // present hook measures the input latency from the input poll to the next present, which only
    // means anything when there is one present per game frame - with the extras it would report a
    // fraction of a frame and take the extra presentations into the average. The patch's own overlay
    // is drawn from that path as well, so it stays on every presentation.
    d3d8_present_bypassing_hooks();
}

// Diagnostics: press U to write the presentation timing of the last frames to oilp_diag.txt (the same
// file name and ring buffer shape the th07 to th20 diagnostics use)
static const unsigned int th6_diag_frames_max = 300;
static const unsigned int th6_diag_presents_max = 8;
struct Th6DiagFrame {
    LARGE_INTEGER base;                                 // the game's own present of the frame
    unsigned int presents;
    LARGE_INTEGER present_time[th6_diag_presents_max];  // when each presentation happened
};
static Th6DiagFrame th6_diag_frames[th6_diag_frames_max];
static unsigned int th6_diag_count;
static bool th6_diag_key_down;
static LARGE_INTEGER th6_diag_perf_freq;

static void th6_diag_dump() {
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

    unsigned int count = th6_diag_count < th6_diag_frames_max ? th6_diag_count : th6_diag_frames_max;
    unsigned int start = th6_diag_count < th6_diag_frames_max ? 0 : th6_diag_count % th6_diag_frames_max;

    fprintf(file, "oilp frame interpolation diagnostics\n");
    fprintf(file, "logic frame rate: %.2f, configured presentation rate: %.2f, display refresh: %uHz\n",
            Limiter::game_fps, Limiter::PresentRate(), Limiter::DisplayRefresh());
    fprintf(file, "configured game frame rate: %u, BltPrepareTime %u, sleep %d\n",
            Config::GameFPS, Config::BltPrepareTime, (int)Config::Sleep);
    fprintf(file, "frames recorded: %u\n", count);
    // The game's own frame rate accounting, from its own side (see Th6FpsState): the text it draws for
    // its frame rate, the pass count it is working on and the two accumulators its "processing rate"
    // comes out of. That count is added to once per draw pass, the extras included, and the two
    // accumulators are added to once per pass while the game is in a menu, so what the game itself
    // makes of them is what says whether putting the state back works: the text should read 60, the two
    // accumulators should be equal (0.00% lost, see ResultScreen.cpp:1670) and - with CalcFPS off - the
    // count should read about 30 (half a second of passes) rather than three times that. With CalcFPS
    // on the game's own counting is replaced (see the comment above), so the count stays cleared and
    // the text carries the frame rate the limiter measured.
    {
        float correction = *(float*)0x006C6ECC;
        float denominator = *(float*)0x006C6ED0;
        float lost = denominator != 0.0f ? (1.0f - (correction / denominator - 0.5f) * 2.0f) * 100.0f : 0.0f;
        fprintf(file, "game's own frame rate: \"%.16s\", count: %u, correction: %.4f/%.4f (%+.2f%%), "
                      "framerate multiplier: %.4f (effective %.4f)\n",
                (char*)0x006C6C08, *(unsigned int*)0x006C6D08, correction, denominator, lost,
                *(float*)0x006C6EC4, *(float*)0x006C6EC0);
    }
    fprintf(file, "frameskip config: %u, cur frame: %u\n",
            (unsigned int)*(unsigned char*)0x006C6E4B,
            (unsigned int)*(unsigned char*)(0x006C6BD4 + 0x10));
    // The bullets (see the stage 1 comment above): what the walk over the game's own 640 slots found,
    // which is what the frame's draw pass draws and what the extras project, and what the manager's
    // own counter says. The counter reads a few higher, and that is the update loop's own doing: it
    // counts a bullet as active before it gets to the end of the switch, so the ones it puts back to
    // BULLET_STATE_INACTIVE on the way there (out of bounds and done despawning) stay in the count it
    // leaves behind.
    fprintf(file, "bullets: %u in play, %u projected forward (last presentation)\n",
            th6_bullets_in_play, th6_bullets_projected);
    fprintf(file, "bullets: the manager's own count says %u (the update's count, taken before it "
                  "turns the bullets it removes off)\n", th6_bullets_managers_say);
    // The player, its orbs and its own shots (see their stage 1 comment above): how far the frame's
    // own pass moved the player and the two orbs, which is what the extras project them by, and what
    // the three states of the shot slots add up to - the drawn ones are the flying and the exploding
    fprintf(file, "player: %.2f px a frame (measured over the frames recorded)\n", th6_player_step_length);
    fprintf(file, "orbs: %.2f/%.2f px a frame (measured; this game shows them from power 8 on)\n",
            th6_orb_step_lengths[0], th6_orb_step_lengths[1]);
    fprintf(file, "shots: %u in play (%u flying, %u exploding), %u projected forward "
                  "(last presentation)\n",
            th6_shots_in_play, th6_shots_flying, th6_shots_exploding, th6_shots_projected);
    // The lasers (see their stage 1 comment above): their slots are in the same manager as the
    // bullets, and the flag that says a slot is in use is the game's own
    fprintf(file, "lasers: %u in play, %u projected forward (last presentation)\n",
            th6_lasers_in_play, th6_lasers_projected);
    double presents = 0.0;
    double first = 0.0;
    double last = 0.0;
    for (unsigned int i = 0; i < count; ++i) {
        const Th6DiagFrame& frame = th6_diag_frames[(start + i) % th6_diag_frames_max];
        fprintf(file, "frame %4u: %u present(s)", i, frame.presents);
        for (unsigned int p = 0; p < frame.presents && p < th6_diag_presents_max; ++p)
            fprintf(file, " [%u] %.2fms", p,
                    (double)(frame.present_time[p].QuadPart - frame.base.QuadPart) * 1000.0 /
                        (double)th6_diag_perf_freq.QuadPart);
        fprintf(file, "\n");
        presents += frame.presents;
        if (i == 0)
            first = (double)frame.base.QuadPart;
        last = (double)frame.base.QuadPart;
    }
    if (last > first && count > 0)
        fprintf(file, "average: %.3f presents per frame, %.1f presentations per second\n",
                presents / (double)count,
                presents * (double)th6_diag_perf_freq.QuadPart / (last - first));

    fclose(file);
    printf("Wrote oilp_diag.txt\n");
}

// Presents the frame the game just drew, and then the extra presentations the config asked for. The
// game calls this from the two exits of GameWindow::Render (0x420A01 and 0x420A0B); both are plain
// calls to the static GameWindow_Present (0x420B50), which takes no arguments (0x420B50 has no
// `mov [ebp-x], ecx`, and its callers push nothing but the device they read themselves).
static void th6_present_hook() {
    if (th6_diag_perf_freq.QuadPart == 0)
        QueryPerformanceFrequency(&th6_diag_perf_freq);

    LARGE_INTEGER base;
    QueryPerformanceCounter(&base);

    // The game's own presentation of this frame, exactly as it always ran it. The back buffer read-back
    // its snapshot key does happens inside it, before any of the extras below draw over it.
    static auto th6_game_present = (void(__cdecl*)())0x00420B50;
    th6_game_present();

    // The game's own work of this frame ends here as far as the overlay is concerned: everything below
    // is the patch presenting the same frame again
    Limiter::MarkFrameWorkEnd();

    // The tool has drawn its frame by now: the other half of watching for its frame state
    ToolState::WatchFrameDrawn();

    LARGE_INTEGER present_time[th6_diag_presents_max] = {};
    unsigned int presents = 1;
    present_time[0] = base;

    // The extras have nothing to draw in a frame the game didn't (see the frame skip note above), and
    // nothing to add while the presents are being thinned out - that is there to keep a frame rate above
    // the display's from being throttled by its presents, and extras would undo it
    if (th6_drew_frame && !Limiter::PresentingEveryNthFrame() && Limiter::ShouldPresent()) {
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

        // Where the game's own part of this frame started, so the extras can tell whether they still fit
        // before the next frame is due
        LARGE_INTEGER frame_start = Limiter::FrameStart();
        LARGE_INTEGER frame_wait = Limiter::FrameWait();
        LARGE_INTEGER game_work;
        game_work.QuadPart = base.QuadPart - frame_start.QuadPart;

        if (extras > 0 && spacing.QuadPart > 0) {
            // The presentations can't run past the end of the frame either: the limiter starts the next
            // frame one frame after this one started, and a frame that overruns it makes it fall behind
            // (and resync). An extra presentation costs about as much as the game's own pass did, so the
            // ones that wouldn't fit are dropped - the frame is then presented fewer times, but never
            // late.
            bool has_deadline = frame_start.QuadPart > 0 && game_work.QuadPart > 0;
            LARGE_INTEGER deadline;
            deadline.QuadPart = frame_start.QuadPart + frame_wait.QuadPart;

            for (unsigned int k = 1; k <= extras; ++k) {
                LARGE_INTEGER target;
                target.QuadPart = base.QuadPart + spacing.QuadPart * (LONGLONG)k;
                if (has_deadline && target.QuadPart + game_work.QuadPart > deadline.QuadPart)
                    continue;

                Limiter::WaitUntil(target);

                if (presents < th6_diag_presents_max)
                    QueryPerformanceCounter(&present_time[presents]);
                ++presents;

                // Where this presentation sits between the game's own one and the next one: the
                // presentations of one game frame are spread evenly over it, so the k-th of extras + 1 of
                // them draws the state k/(extras + 1) frames along
                th6_extra_presentation((float)k / (float)(extras + 1));
            }
        }
    }

    th6_drew_frame = false;

    // Record the frame for the diagnostics
    {
        Th6DiagFrame& frame = th6_diag_frames[th6_diag_count % th6_diag_frames_max];
        frame.base = base;
        frame.presents = presents;
        for (unsigned int p = 0; p < th6_diag_presents_max; ++p)
            frame.present_time[p] = p < presents ? present_time[p] : present_time[0];
        ++th6_diag_count;
    }

    // U dumps the diagnostics
    if (GetAsyncKeyState('U') & 0x8000) {
        if (!th6_diag_key_down) {
            th6_diag_key_down = true;
            th6_diag_dump();
        }
    } else {
        th6_diag_key_down = false;
    }
}

// Takes over the two present calls the frame ends with
static void th6_install_interpolation() {
    {
        // call 0x420B50, GameWindow_Present (0x420A01) - the exit the fullscreen path takes when the
        // game's own frame skip setting says enough of this frame has been drawn, which then goes round
        // to draw again
        BYTE* site = (BYTE*)0x00420A01;
        if (site[0] == 0xE8 && *(DWORD*)(site + 1) == 0x00420B50 - (0x00420A01 + 5)) {
            patch_call(0x00420A01, th6_present_hook);
        } else {
            printf("th06 present call at 0x420A01 doesn't look like a known one, skipping!\n");
        }
    }
    {
        // call 0x420B50, GameWindow_Present (0x420A0B) - the exit a normal frame takes, which ends the
        // frame there (effectiveFramerateMultiplier, curFrame = 0, g_TickCountToEffectiveFramerate++)
        BYTE* site = (BYTE*)0x00420A0B;
        if (site[0] == 0xE8 && *(DWORD*)(site + 1) == 0x00420B50 - (0x00420A0B + 5)) {
            patch_call(0x00420A0B, th6_present_hook);
        } else {
            printf("th06 present call at 0x420A0B doesn't look like a known one, skipping!\n");
        }
    }
    {
        // The overlay's readouts are Config::ShowOverlay's business rather than the interpolation's,
        // and which of them are shown is set once in Limiter::Initialize: the rate the frames are
        // actually presented at is counted by the D3D8 hook, which sees every present, so it works
        // with the interpolation off as well.
    }

    // From here on the calculation chain hook draws the frame (see the stage 0 comment above)
    th6_interpolating = true;

    printf("Frame interpolation: th06 extra presentations installed (setting %d)\n", Config::Interpolation);
}
