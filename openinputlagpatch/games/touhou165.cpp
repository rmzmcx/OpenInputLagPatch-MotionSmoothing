// Touhou 16.5: Violet Detector v1.00a

#include <stdio.h>
#include "patch_util.h"
#include "limiter.h"
#include "d3d9_hook.h"
#include "config.h"
#include "touhou165.h"

using namespace Touhou165;

CReplayManager** CReplayManager::InstancePtr = (CReplayManager**)0x004B565C;

static unsigned int th165_window_update_calls;

int __fastcall th165_window_update_hook(void* self);

// Set by th165_install_bullet_pixel_snapping: 4 when the four frndint were replaced, -1 when a site
// did not match. Only the dump reads it.
static int th165_pixel_snapping_patched;

// ---------------------------------------------------------------------------
// Bullet pixel snapping
// ---------------------------------------------------------------------------
//
// sub_46B6C0 rounds a sprite's four quad vertices to whole pixels when bit 0 of its flags argument
// is set, and the four frndint at 0x46B799, 0x46B7A7, 0x46B7B5 and 0x46B7C3 each feed an fsub 0.5
// that turns the rounded value into a pixel boundary. Turning the rounding off leaves the
// subtraction, so what used to be round(v) - 0.5 becomes v - 0.5 - the same quad on its fraction.
//
// Like th13 through th19 this game has no second layer: the submit function builds the vertices
// itself from the sprite's own float position, and nothing on that path calls the CRT's floor.
//
// No copy of the sprite either: bullet.anm's atlases are all Format 1 (A8R8G8B8), whose 8-bit
// antialiased edge absorbs a sample that crosses the sprite rectangle.
static void th165_install_bullet_pixel_snapping() {
	static const unsigned int rounding[] = { 0x0046B799, 0x0046B7A7, 0x0046B7B5, 0x0046B7C3 };
	th165_pixel_snapping_patched = -1;
	for (unsigned int i = 0; i < 4; i++) {
		if (*(BYTE*)rounding[i] != 0xD9 || *(BYTE*)(rounding[i] + 1) != 0xFC) {
			printf("th165 sprite rounding at 0x%08X doesn't look like a known one, skipping!\n", rounding[i]);
			return;
		}
	}
	for (unsigned int i = 0; i < 4; i++) {
		BYTE nop[2] = { 0x90, 0x90 };
		patch_bytes(rounding[i], nop, sizeof(nop));
	}
	th165_pixel_snapping_patched = 4;
	printf("th165 sprites drawn at their fractional position (rounding dropped, half-pixel kept)\n");
}

void th165_install_patches() {
	{
		// Skip the original frame limiter
		BYTE patch[] = { 0xEB, 0x51 };
		patch_bytes(0x00460FA7, patch, sizeof(patch));
	}
	{
		// Force fast input latency mode
		BYTE patch[] = { 0xE9, 0xE9, 0x01, 0x00, 0x00, 0x90 };
		patch_bytes(0x004602BD, patch, sizeof(patch));
	}
	{
		// Hook window update
		patch_call(0x004604B0, th165_window_update_hook);
	}
	if (Config::D3D9Ex) {
		// Redirect Direct3DCreate9 call
		// IAT is being hooked, but thcrap also hooks Direct3DCreate9 in the same place
		// This should force our Direct3DCreate9 hook to be loaded no matter what
		patch_call(0x00460037, Direct3DCreate9_hook);

		// Extra NOP is needed because we're replacing a FF 15 call, which is 6 bytes long
		BYTE patch[] = { 0x90 };
		patch_bytes(0x0046003C, patch, sizeof(patch));
	}
	if (Config::ReplaySpeedControl) {
		// Skip the original replay speed control stuff
		BYTE patch[] = { 0xEB };
		patch_bytes(0x0045053C, patch, sizeof(patch));
	}
	if (Config::FixBulletPixelSnapping)
		th165_install_bullet_pixel_snapping();
}

void th165_diag_dump() {
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

	// This game has no frame interpolation yet, so the dump carries what the patch itself can
	// answer: whether the window hook has run, whether a D3D9 device is being tracked, the rates the
	// limiter works with, and whether the rounding was dropped.
	fprintf(file, "oilp diagnostics\n");
	fprintf(file, "window update calls: %u\n", th165_window_update_calls);
	fprintf(file, "D3D9 device: %s\n", d3d9_hooked_device() != nullptr ? "created" : "not yet");
	fprintf(file, "logic frame rate: %.2f, configured presentation rate: %.2f\n",
		Limiter::game_fps, Limiter::PresentRate());
	// 90 90 means the submit really draws at the fractional position. 0 means the install never
	// ran, -1 means a site did not match.
	fprintf(file, "pixel snapping: %d of 4 frndint replaced, 0x0046B799 now holds %02X %02X\n",
		th165_pixel_snapping_patched,
		*(unsigned char*)0x0046B799, *(unsigned char*)0x0046B79A);

	fclose(file);
	printf("Wrote oilp_diag.txt\n");
}

int __fastcall th165_window_update_hook(void* self) {
	++th165_window_update_calls;
	Limiter::Tick();
	return CWindowManager__UpdateFast(self);
}

FPSTarget th165_replay_callback() {
	if (*CReplayManager::InstancePtr && (*CReplayManager::InstancePtr)->mode == 1) {
		// TODO: Reverse engineer this struct instead of being lazy
		auto input = *(DWORD*)0x004B0FF0;
		if (input & InputState::Focus)
			return FPSTarget::ReplaySlow;
		else if (input & (InputState::Skip | InputState::Shoot))
			return FPSTarget::ReplaySkip;
	}
	return FPSTarget::Game;
}
