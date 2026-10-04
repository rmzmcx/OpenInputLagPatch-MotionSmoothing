#pragma once

#include <Windows.h>

// IDirect3DDevice8 comes from the D3D8 headers in d3d8/, which can't be included in the
// same translation unit as the D3D9 headers the Windows SDK ships, so it's only forward
// declared here and included by the files that actually need it
struct IDirect3DDevice8;

namespace D3D8Hook {
	// IAT hooks Direct3DCreate8 so the game's IDirect3D8::CreateDevice call can be
	// intercepted. This is where window mode is forced for D3D8 titles, which also covers
	// wrappers that never go through d3d9.dll (dgVoodoo2 in particular).
	void HookDirect3DCreate8();

	// Hooks IDirect3DDevice8::Present (vtable index 15) so frames can be skipped when the
	// target framerate is above the display refresh rate (see Limiter::ShouldPresent), and
	// IDirect3DDevice8::Reset (vtable index 14) so window mode survives a device reset.
	// Safe to call every frame: it does nothing until a device is available, and only
	// installs the hook once.
	void Install(IDirect3DDevice8* device);
}
