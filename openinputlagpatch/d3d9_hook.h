#pragma once
#include <d3d9.h>

IDirect3D9* WINAPI Direct3DCreate9_hook(UINT SDKVersion);
void hook_d3d9();

// The D3D9 device the patch hooked, or null while the game doesn't have one
IDirect3DDevice9* d3d9_hooked_device();

// Presents with the Present that was in place when the device was created, i.e. without the hooks
// that other patches and tools put in front of it (see the comment in d3d9_hook.cpp)
HRESULT d3d9_present_bypassing_hooks();

struct D3D9PresentInfo {
	unsigned int width;
	unsigned int height;
	unsigned int back_buffer_count;
	unsigned int swap_effect;
	unsigned int presentation_interval;
	unsigned int refresh_rate;
	bool windowed;
	bool d3d9ex;
};

D3D9PresentInfo d3d9_get_present_info();
