#pragma once
#include <d3d9.h>

static IDirect3DDevice9* d3d9_device;

IDirect3D9* WINAPI Direct3DCreate9_hook(UINT SDKVersion);
void hook_d3d9();

// The D3D9 device the patch hooked, or null while the game doesn't have one
IDirect3DDevice9* d3d9_hooked_device();

// Presents with the Present that was in place when the device was created, i.e. without the hooks
// that other patches and tools put in front of it (see the comment in d3d9_hook.cpp)
HRESULT d3d9_present_bypassing_hooks();
