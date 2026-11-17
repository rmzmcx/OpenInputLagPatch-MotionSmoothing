// Touhou 9: Phantasmagoria of Flower View v1.50a

#include <stdio.h>
#include "patch_util.h"
#include "limiter.h"
#include "d3d8_hook.h"
#include "touhou9.h"
#include "config.h"

using namespace Touhou9;

CEngine* CEngine::Instance = (CEngine*)0x004B3100;
CGame* CGame::Instance = (CGame*)0x004A7D90;

static unsigned int th9_window_update_calls;

int __fastcall th9_window_update_hook(void* self);

// Set by th9_install_bullet_pixel_snapping: 4 when the four frndint were replaced, -1 when a site
// did not match. Only the dump reads it.
static int th9_pixel_snapping_patched;

// ---------------------------------------------------------------------------
// Bullet pixel snapping
// ---------------------------------------------------------------------------
//
// sub_43A0F0 draws a sprite's four quad vertices and rounds them to whole pixels when bit 0 of the
// flags argument is set: 0x43A106 tests that bit, 0x43A178 is the branch, and the four frndint at
// 0x43A180, 0x43A18E, 0x43A19C and 0x43A1AA each feed an fsub 0.5 (flt_490F54) that turns the
// rounded value into a pixel boundary. This is the game's no-rotation draw, so it is where every
// bullet's position gets quantised. The four frndint become NOPs and the fsub 0.5 stays, turning
// round(v) - 0.5 into v - 0.5.
//
// Dropping the rounding is only half of it here. The atlas is A4R4G4B4 - 16 alpha levels - and the
// sprites sit edge to edge (etama.png: 168 sprites, 13.8% of the edge texels partially transparent,
// the same numbers th07 and th08 report), so a sprite drawn at a fractional position samples past
// its own rectangle and picks up its neighbour. That is the streaking.

// ---------------------------------------------------------------------------
// One texture per sprite
// ---------------------------------------------------------------------------
//
// th06's answer, which is the one that fits this game: give the sprite a texture of its own, run
// the UV over the whole of it, and set the address mode to CLAMP so a sample that strays past 0 or
// 1 is pinned to the sprite's own edge instead of reaching the neighbour. Nothing is squeezed and
// the pixels are copied unchanged.
//
// It has to be this and not th07/th08's premultiplied copy, because this game does not blend with
// SRCALPHA: sub_43A030 picks the source blend from vm+0x1F8 bits 4-5 and sets either ONE or
// INVSRCALPHA. Premultiplying the copy and switching the blend to ONE - which is what makes the
// th07/th08 copies exact - would change what those two blends produce.
//
// Only sprites up to 32 pixels get a texture: the bullet atlases hold nothing larger, and leaving
// the big sheets on the atlas keeps the texture count down.

static const unsigned int th9_vm_sprite = 0x224;
static const unsigned int th9_sprite_size = 0x38;
static const unsigned int th9_copy_max = 256;
static const int th9_copy_max_pixels = 32;

struct Th9SpriteCopy {
    unsigned char* original;
    unsigned char* copy;
    IDirect3DTexture8* atlas;
};

static Th9SpriteCopy th9_copies[th9_copy_max];
static unsigned int th9_copy_count;
static int th9_copy_failure;    // why the last creation gave up, for the diagnostics
static bool th9_copies_enabled; // set once the patch installed
static int th9_current_is_copy; // the sprite this draw uses came from a copy

static unsigned char* th9_build_copy(unsigned char* sprite) {
    IDirect3DTexture8* atlas = *(IDirect3DTexture8**)(sprite + 0x04);
    if (atlas == nullptr) {
        th9_copy_failure = 2;
        return nullptr;
    }

    D3DSURFACE_DESC description;
    if (atlas->GetLevelDesc(0, &description) != D3D_OK) {
        th9_copy_failure = 10;
        return nullptr;
    }

    // The sprite stores UV rather than pixels - LoadSprite divided the pixel rectangle by the atlas
    // size - so this is that division undone
    const float atlas_width = (float)description.Width;
    const float atlas_height = (float)description.Height;
    const int left = (int)(*(float*)(sprite + 0x20) * atlas_width + 0.5f);
    const int top = (int)(*(float*)(sprite + 0x24) * atlas_height + 0.5f);
    const int width = (int)((*(float*)(sprite + 0x28) - *(float*)(sprite + 0x20)) * atlas_width + 0.5f);
    const int height = (int)((*(float*)(sprite + 0x2C) - *(float*)(sprite + 0x24)) * atlas_height + 0.5f);
    if (width <= 0 || height <= 0 || width > th9_copy_max_pixels || height > th9_copy_max_pixels) {
        th9_copy_failure = 3;
        return nullptr;
    }

    unsigned int bpp;
    switch (description.Format) {
    case D3DFMT_A8R8G8B8:
    case D3DFMT_X8R8G8B8:
        bpp = 4;
        break;
    case D3DFMT_A4R4G4B4:
    case D3DFMT_A1R5G5B5:
    case D3DFMT_R5G6B5:
        bpp = 2;
        break;
    default:
        th9_copy_failure = 4;
        return nullptr;
    }

    IDirect3DDevice8* device = CEngine::Instance->d3d8_device;
    if (device == nullptr) {
        th9_copy_failure = 11;
        return nullptr;
    }

    IDirect3DTexture8* texture = nullptr;
    if (device->CreateTexture(width, height, 1, 0, description.Format, D3DPOOL_MANAGED, &texture) != D3D_OK) {
        th9_copy_failure = 12;
        return nullptr;
    }

    D3DLOCKED_RECT source_rect;
    if (atlas->LockRect(0, &source_rect, nullptr, D3DLOCK_READONLY) != D3D_OK) {
        texture->Release();
        th9_copy_failure = 13;
        return nullptr;
    }
    D3DLOCKED_RECT target_rect;
    if (texture->LockRect(0, &target_rect, nullptr, 0) != D3D_OK) {
        atlas->UnlockRect(0);
        texture->Release();
        th9_copy_failure = 5;
        return nullptr;
    }

    for (int y = 0; y < height; y++) {
        const int sy = top + y;
        if (sy < 0 || sy >= (int)description.Height)
            continue;
        const unsigned char* source_row = (const unsigned char*)source_rect.pBits + sy * source_rect.Pitch;
        unsigned char* target_row = (unsigned char*)target_rect.pBits + y * target_rect.Pitch;
        for (int x = 0; x < width; x++) {
            const int sx = left + x;
            if (sx < 0 || sx >= (int)description.Width)
                continue;
            memcpy(target_row + x * bpp, source_row + sx * bpp, bpp);
        }
    }

    atlas->UnlockRect(0);
    texture->UnlockRect(0);

    unsigned char* copy = (unsigned char*)malloc(th9_sprite_size);
    if (copy == nullptr) {
        texture->Release();
        th9_copy_failure = 8;
        return nullptr;
    }
    memcpy(copy, sprite, th9_sprite_size);
    // The UV covers the copy edge to edge: a whole position lands the samples on texel centres, a
    // fractional one mixes inside this sprite only, because CLAMP pins whatever strays past 0 and 1
    *(IDirect3DTexture8**)(copy + 0x04) = texture;
    *(float*)(copy + 0x20) = 0.0f;
    *(float*)(copy + 0x24) = 0.0f;
    *(float*)(copy + 0x28) = 1.0f;
    *(float*)(copy + 0x2C) = 1.0f;

    th9_copies[th9_copy_count].original = sprite;
    th9_copies[th9_copy_count].copy = copy;
    th9_copies[th9_copy_count].atlas = atlas;
    th9_copy_count++;
    return copy;
}

static unsigned char* th9_copy_for(unsigned char* sprite) {
    if (!th9_copies_enabled)
        return nullptr;
    for (unsigned int i = 0; i < th9_copy_count; i++) {
        // The sprite the vm carries after the first replacement is the copy itself
        if (th9_copies[i].copy == sprite)
            return sprite;
        if (th9_copies[i].original == sprite) {
            // A stage change frees and reloads the sprite arrays, and a reloaded one can land on an
            // address an older copy was taken from. The texture is what says whether it is still the
            // same sprite
            if (th9_copies[i].atlas == *(IDirect3DTexture8**)(sprite + 0x04))
                return th9_copies[i].copy;
            memset(&th9_copies[i], 0, sizeof(th9_copies[i]));
            break;
        }
    }
    if (th9_copy_count >= th9_copy_max) {
        th9_copy_failure = 9;
        return nullptr;
    }
    return th9_build_copy(sprite);
}

static void th9_bind(unsigned char* vm) {
    th9_current_is_copy = 0;
    if (vm == nullptr)
        return;
    unsigned char* sprite = *(unsigned char**)(vm + th9_vm_sprite);
    if (sprite == nullptr)
        return;
    unsigned char* copy = th9_copy_for(sprite);
    if (copy == nullptr)
        return;
    *(unsigned char**)(vm + th9_vm_sprite) = copy;
    th9_current_is_copy = 1;
}

// The address mode is per-stage rather than per-texture in D3D8, so it goes back to WRAP for
// everything that still draws from an atlas
static void th9_set_address_mode() {
    IDirect3DDevice8* device = CEngine::Instance->d3d8_device;
    if (device == nullptr)
        return;
    const DWORD mode = th9_current_is_copy ? D3DTADDRESS_CLAMP : D3DTADDRESS_WRAP;
    device->SetTextureStageState(0, D3DTSS_ADDRESSU, mode);
    device->SetTextureStageState(0, D3DTSS_ADDRESSV, mode);
}

// 0x43A0F0 is `push ebp; mov ebp, esp; sub esp, 0Ch` (6 bytes), replaced by a 5-byte jump and a
// NOP. The hook replays those three instructions and resumes at 0x43A0F6.
static __declspec(naked) void th9_draw_hook() {
    __asm {
        pushad
        mov eax, [esp + 32 + 4]   // arg1, the AnmVm
        push eax
        call th9_bind
        add esp, 4
        popad
        push ebp
        mov ebp, esp
        sub esp, 0Ch
        mov eax, 0043A0F6h
        jmp eax
    }
}

// 0x43A504 is `push offset flt_4DC558` (5 bytes), the step between setting the blend and submitting
// the quad, so the address mode is right for the draw that follows
static __declspec(naked) void th9_address_hook() {
    __asm {
        pushad
        call th9_set_address_mode
        popad
        push 4DC558h
        mov eax, 0043A509h
        jmp eax
    }
}

static void th9_install_bullet_pixel_snapping() {
    static const unsigned int rounding[] = { 0x0043A180, 0x0043A18E, 0x0043A19C, 0x0043A1AA };
    static const BYTE prologue[] = { 0x55, 0x8B, 0xEC, 0x83, 0xEC, 0x0C };
    static const BYTE push_site[] = { 0x68, 0x58, 0xC5, 0x4D, 0x00 };
    th9_pixel_snapping_patched = -1;
    for (unsigned int i = 0; i < 4; i++) {
        if (*(BYTE*)rounding[i] != 0xD9 || *(BYTE*)(rounding[i] + 1) != 0xFC) {
            printf("th09 sprite rounding at 0x%08X doesn't look like a known one, skipping!\n", rounding[i]);
            return;
        }
    }
    for (unsigned int i = 0; i < 6; i++) {
        if (*(BYTE*)(0x0043A0F0 + i) != prologue[i]) {
            printf("th09 draw entry doesn't look like a known one, skipping!\n");
            return;
        }
    }
    for (unsigned int i = 0; i < 5; i++) {
        if (*(BYTE*)(0x0043A504 + i) != push_site[i]) {
            printf("th09 blend site doesn't look like a known one, skipping!\n");
            return;
        }
    }
    if (CEngine::Instance->d3d8_device == nullptr) {
        printf("th09 has no D3D8 device yet, sprites keep the atlas\n");
        return;
    }

    for (unsigned int i = 0; i < 4; i++) {
        BYTE nop[2] = { 0x90, 0x90 };
        patch_bytes(rounding[i], nop, sizeof(nop));
    }
    patch_jmp(0x0043A0F0, th9_draw_hook);
    BYTE nop[] = { 0x90 };
    patch_bytes(0x0043A0F5, nop, sizeof(nop));
    patch_jmp(0x0043A504, th9_address_hook);

    th9_copies_enabled = true;
    th9_pixel_snapping_patched = 4;
    printf("th09 sprites drawn at their fractional position (rounding dropped, one texture per sprite)\n");
}

void th9_install_patches() {
    {
        // Leave the loop as soon as the frame is due: the game's own wait loop runs at
        // 0x42DC74..0x42DC8D, and 0x42DC72 is the 2-byte jnz that enters it. Only the opcode is
        // rewritten (75 -> EB), so the displacement 19h stays and the jump lands on the loop's
        // exit at 0x42DC8D instead of its body.
        BYTE patch[] = { 0xEB };
        patch_bytes(0x0042DC72, patch, sizeof(patch));
    }
    {
        // Don't sleep in the main loop: 0x42DC64 is the 6-byte near jnz that branches to the
        // Sleep path at 0x42DD77 when the frame is not due yet. NOPing it keeps the loop running
        // instead of yielding the rest of the timeslice.
        BYTE patch[] = { 0x90, 0x90, 0x90, 0x90, 0x90, 0x90 };
        patch_bytes(0x0042DC64, patch, sizeof(patch));
    }
    {
        // Hook window update: 0x42E6A6 is `call sub_42DC40` (CWindowManager__Update), with the
        // window manager in ecx (offset hWnd, 0x4B30B0) - the same shape the hook takes.
        patch_call(0x0042E6A6, th9_window_update_hook);
    }
}

void th9_diag_dump() {
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

    fprintf(file, "oilp diagnostics\n");
    fprintf(file, "window update calls: %u\n", th9_window_update_calls);
    fprintf(file, "D3D8 device: %s\n",
        CEngine::Instance->d3d8_device != nullptr ? "created" : "not yet");
    fprintf(file, "logic frame rate: %.2f, configured presentation rate: %.2f\n",
        Limiter::game_fps, Limiter::PresentRate());
    // Whether the rounding was dropped: 90 90 means the draw really runs at the fractional
    // position. 0 means the install never ran, -1 means a site did not match.
    fprintf(file, "pixel snapping: %d of 4 frndint replaced, 0x0043A180 now holds %02X %02X\n",
        th9_pixel_snapping_patched,
        *(unsigned char*)0x0043A180, *(unsigned char*)0x0043A181);
    // One texture per sprite: how many were made, and the last reason one was refused. 0 is "no
    // failure so far"; 3 means the sprite was larger than the 32-pixel cap, 9 means the table filled.
    fprintf(file, "sprite copies: %u made, last failure %d\n", th9_copy_count, th9_copy_failure);

    fclose(file);
    printf("Wrote oilp_diag.txt\n");
}

int __fastcall th9_window_update_hook(void* self) {
    // The D3D8 Present hook has to be installed from here rather than during process
    // initialisation: doing it early leaves the D3D8 titles stuck before their main loop (the same
    // reason th95 waits for its first window update). Install does nothing until a device exists
    // and only hooks once, so calling it every frame is what the interface expects.
    IDirect3DDevice8* device = CEngine::Instance->d3d8_device;
    if (device != nullptr) {
        D3D8Hook::Install(device);
        // The per-sprite textures need the device that was just created, so the snapping goes in
        // here rather than in th9_install_patches
        static bool snapping_attempted = false;
        if (!snapping_attempted) {
            snapping_attempted = true;
            if (Config::FixBulletPixelSnapping)
                th9_install_bullet_pixel_snapping();
        }
    }

    ++th9_window_update_calls;
    Limiter::Tick();
    return CWindowManager__Update(self);
}

FPSTarget th9_replay_callback() {
    if (CGame::Instance->replay && CGame::Instance->replay->is_replay) {
        auto input = get_input(2);
        if (input & InputState::Focus)
            return FPSTarget::ReplaySlow;
        else if (input & (InputState::Skip | InputState::Shoot))
            return FPSTarget::ReplaySkip;
    }
    return FPSTarget::Game;
}
