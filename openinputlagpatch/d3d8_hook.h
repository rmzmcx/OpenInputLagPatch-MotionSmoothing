#pragma once

#include <Windows.h>

// IDirect3DDevice8 comes from the D3D8 headers in d3d8/, which can't be included in the
// same translation unit as the D3D9 headers the Windows SDK ships, so it's only forward
// declared here and included by the files that actually need it
struct IDirect3DDevice8;

namespace D3D8Hook {
	typedef void (*DeviceResetCallback)(IDirect3DDevice8* device, void* caller);
	typedef void (*Direct3DCreateCallback)();

	struct PresentInfo {
		unsigned int width;
		unsigned int height;
		unsigned int back_buffer_count;
		unsigned int swap_effect;
		unsigned int presentation_interval;
		bool windowed;
	};

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

	// Called after a successful IDirect3DDevice8::Reset. The th08-equivalent thprac hook uses a
	// game-side device recreation point to reinitialize the tool against the new device.
	void SetDeviceResetCallback(DeviceResetCallback callback);

	// Called before the game creates its IDirect3D8 object. This is the last point after the game
	// has loaded its own config but before it builds presentation parameters or a window.
	void SetDirect3DCreateCallback(Direct3DCreateCallback callback);

	PresentInfo GetPresentInfo();
}

// Presents with the Present that was in place when the device was created, i.e. without the hooks
// that other patches and tools put in front of it (the frame interpolation presents its extra
// frames with this, see the comment in d3d9_hook.cpp). The patch's own overlay is drawn from here
// as well, since this path goes around the Present hook that normally draws it.
HRESULT d3d8_present_bypassing_hooks();
