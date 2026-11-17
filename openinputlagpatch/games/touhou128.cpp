// Touhou 12.8: Great Fairy Wars v1.00a

#include "patch_util.h"
#include "limiter.h"
#include "d3d9_hook.h"
#include "config.h"
#include "touhou128.h"

using namespace Touhou128;

CReplayManager** CReplayManager::InstancePtr = (CReplayManager**)0x004B8A84;

int __stdcall th128_window_update_hook(void* self) {
	Limiter::Tick();
	return CWindowManager__UpdateFast(self);
}

// Set by th128_install_bullet_pixel_snapping: 4 when the four frndint were replaced, -1 when a site
// did not match. Only the dump reads it.
static int th128_pixel_snapping_patched;

void th128_diag_dump() {
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

	// This game has no frame interpolation yet, so the dump carries only what it can answer: which
	// of the two rounding sites the pixel-snapping patch reached. 4 with 90 90 and DD 04 24 means
	// both layers are off; 0 means the install never ran, -1 means a site did not match.
	fprintf(file, "oilp diagnostics\n");
	fprintf(file, "pixel snapping: %d of 4 frndint replaced, 0x45E9C9 now holds %02X %02X, "
				  "floor at 0x45F25D now holds %02X %02X %02X\n",
		th128_pixel_snapping_patched,
		*(unsigned char*)0x0045E9C9, *(unsigned char*)0x0045E9CA,
		*(unsigned char*)0x0045F25D, *(unsigned char*)0x0045F25E, *(unsigned char*)0x0045F25F);

	fclose(file);
	printf("Wrote oilp_diag.txt\n");
}

// ---------------------------------------------------------------------------
// Bullet pixel snapping
// ---------------------------------------------------------------------------
//
// Two layers, as in th10 through th125. sub_45F110 builds the four vertices and calls the CRT's
// floor (0x48FDF0) twice - 0x45F25D for the left edge, 0x45F35D for the top edge, both preceded by
// fstp qword [esp] (DD 1C 24) - and sub_45E920 then rounds what came out, with the four frndint at
// 0x45E9C9, 0x45E9D7, 0x45E9E5 and 0x45E9F3, each followed by an fsub 0.5 and gated on test bl, 1.
//
// Both layers have to go. Dropping only the frndint leaves the vertices whole, and dropping only
// the floor would leave the frndint putting the rounding back. The two calls become
// fld qword ptr [esp] plus two NOPs, and the four frndint become NOPs.
//
// No copy of the sprite: bullet.anm's main atlas is A8R8G8B8 with an 8-bit antialiased edge.
static void th128_install_bullet_pixel_snapping() {
	static const unsigned int floor_sites[] = { 0x0045F25D, 0x0045F35D };
	static BYTE drop_floor[] = { 0xDD, 0x04, 0x24, 0x90, 0x90 };
	static const unsigned int rounding[] = { 0x0045E9C9, 0x0045E9D7, 0x0045E9E5, 0x0045E9F3 };
	th128_pixel_snapping_patched = -1;
	for (unsigned int i = 0; i < 2; i++) {
		const BYTE* site = (const BYTE*)floor_sites[i];
		if (site[0] != 0xE8 || *(const DWORD*)(site + 1) != 0x0048FDF0 - floor_sites[i] - 5) {
			printf("th128 floor call at 0x%08X doesn't look like a known one, skipping!\n", floor_sites[i]);
			return;
		}
	}
	for (unsigned int i = 0; i < 4; i++) {
		if (*(BYTE*)rounding[i] != 0xD9 || *(BYTE*)(rounding[i] + 1) != 0xFC) {
			printf("th128 sprite rounding at 0x%08X doesn't look like a known one, skipping!\n", rounding[i]);
			return;
		}
	}
	for (unsigned int i = 0; i < 2; i++)
		patch_bytes(floor_sites[i], drop_floor, sizeof(drop_floor));
	for (unsigned int i = 0; i < 4; i++) {
		BYTE nop[2] = { 0x90, 0x90 };
		patch_bytes(rounding[i], nop, sizeof(nop));
	}
	printf("th128 sprites drawn at their fractional position (two floor calls and four frndint dropped)\n");
	th128_pixel_snapping_patched = 4;
}

void th128_install_patches() {
	{
		// Skip the original frame limiter
		BYTE patch[] = { 0xEB, 0x5B };
		patch_bytes(0x00454C14, patch, sizeof(patch));
	}
	{
		// Force fast input latency mode
		BYTE patch[] = { 0xEB };
		patch_bytes(0x00453D42, patch, sizeof(patch)); // Skip over automatic
		patch_bytes(0x00453D56, patch, sizeof(patch)); // Skip over normal
	}
	{
		// Hook window update
		patch_call(0x00453D72, th128_window_update_hook);
	}
	if (Config::D3D9Ex) {
		// Redirect Direct3DCreate9 call
		// IAT is being hooked, but thcrap also hooks Direct3DCreate9 in the same place
		// This should force our Direct3DCreate9 hook to be loaded no matter what
		patch_call(0x00453B8B, Direct3DCreate9_hook);

		// Extra NOP is needed because we're replacing a FF 15 call, which is 6 bytes long
		BYTE patch[] = { 0x90 };
		patch_bytes(0x00453B90, patch, sizeof(patch));
	}
	if (Config::ReplaySpeedControl) {
		// Skip the original replay speed control stuff
		BYTE patch[] = { 0xEB };
		patch_bytes(0x0044204F, patch, sizeof(patch));
	}
	if (Config::FixBulletPixelSnapping)
		th128_install_bullet_pixel_snapping();
}

FPSTarget th128_replay_callback() {
	if (*CReplayManager::InstancePtr && (*CReplayManager::InstancePtr)->mode == 1) {
		// TODO: Reverse engineer this struct instead of being lazy
		auto input = *(DWORD*)0x004DAFF8;
		if (input & InputState::Focus)
			return FPSTarget::ReplaySlow;
		else if (input & (InputState::Skip | InputState::Shoot))
			return FPSTarget::ReplaySkip;
	}
	return FPSTarget::Game;
}