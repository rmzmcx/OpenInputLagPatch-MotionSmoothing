#include <stdio.h>

#include "d3d8_hook.h"
#include "d3d8/d3d8.h"
#include "limiter.h"
#include "patch_util.h"
#include "window_mode.h"

namespace {
	typedef HRESULT(__stdcall* CreateDevice_t)(IDirect3D8*, UINT, D3DDEVTYPE, HWND, DWORD, D3DPRESENT_PARAMETERS*, IDirect3DDevice8**);
	typedef HRESULT(__stdcall* Reset_t)(IDirect3DDevice8*, D3DPRESENT_PARAMETERS*);
	typedef HRESULT(__stdcall* Present_t)(IDirect3DDevice8*, const RECT*, const RECT*, HWND, const RGNDATA*);
	typedef IDirect3D8* (WINAPI* Direct3DCreate8_t)(UINT);

	CreateDevice_t CreateDevice_orig = nullptr;
	Reset_t Reset_orig = nullptr;
	Present_t Present_orig = nullptr;
	Direct3DCreate8_t Direct3DCreate8_orig = nullptr;
	bool device_hooks_installed = false;
	bool create8_hooked = false;

	// Forces the presentation parameters into the mode that was picked on boot, if the
	// window mode question is enabled
	void apply_window_override(D3DPRESENT_PARAMETERS* params) {
		switch (WindowMode::GetEffectiveOverride()) {
		case WindowMode::Override::Windowed:
			params->Windowed = TRUE;
			break;
		case WindowMode::Override::Fullscreen:
			params->Windowed = FALSE;
			break;
		}
	}

	HRESULT __stdcall Present_hook(IDirect3DDevice8* device, const RECT* src_rect, const RECT* dst_rect,
	                               HWND dst_window, const RGNDATA* dirty_region) {
		if (!Limiter::ShouldPresent())
			return D3D_OK;
		return Present_orig(device, src_rect, dst_rect, dst_window, dirty_region);
	}

	HRESULT __stdcall Reset_hook(IDirect3DDevice8* device, D3DPRESENT_PARAMETERS* present_params) {
		printf("D3D8 Reset intercepted!\n");
		apply_window_override(present_params);

		auto ret = Reset_orig(device, present_params);

		if (SUCCEEDED(ret)) {
			D3DDEVICE_CREATION_PARAMETERS creation_params = {};
			device->GetCreationParameters(&creation_params);
			WindowMode::Apply(
				present_params->hDeviceWindow ? present_params->hDeviceWindow : creation_params.hFocusWindow,
				present_params->Windowed
			);
		}

		return ret;
	}

	HRESULT __stdcall CreateDevice_hook(IDirect3D8* self, UINT Adapter, D3DDEVTYPE DeviceType, HWND hFocusWindow,
	                                    DWORD BehaviorFlags, D3DPRESENT_PARAMETERS* pp, IDirect3DDevice8** ppReturnedDeviceInterface) {
		printf("D3D8 CreateDevice intercepted!\n");
		apply_window_override(pp);

		auto ret = CreateDevice_orig(self, Adapter, DeviceType, hFocusWindow, BehaviorFlags, pp, ppReturnedDeviceInterface);

		if (SUCCEEDED(ret) && ppReturnedDeviceInterface != nullptr && *ppReturnedDeviceInterface != nullptr) {
			D3D8Hook::Install(*ppReturnedDeviceInterface);
			WindowMode::Apply(hFocusWindow ? hFocusWindow : pp->hDeviceWindow, pp->Windowed);
		}

		return ret;
	}

	IDirect3D8* WINAPI Direct3DCreate8_hook(UINT SDKVersion) {
		printf("Direct3DCreate8 intercepted!\n");

		// Window mode: this runs after the game read its config but before it creates its
		// window, which is the point where the game's own windowed flag has to be set
		WindowMode::OnD3DCreate();

		auto d3d8 = Direct3DCreate8_orig(SDKVersion);
		if (d3d8 == nullptr)
			return nullptr;

		// Hook IDirect3D8::CreateDevice (vtable index 15)
		DWORD* vtbl = *(DWORD**)d3d8;
		auto hook = (DWORD)CreateDevice_hook;
		if (vtbl[15] == hook)
			return d3d8;

		// The object's vtable is shared, so the original function may only be captured once
		if (CreateDevice_orig == nullptr)
			CreateDevice_orig = (CreateDevice_t)vtbl[15];
		patch_bytes(&vtbl[15], &hook, sizeof(DWORD));

		return d3d8;
	}
}

void D3D8Hook::Install(IDirect3DDevice8* device) {
	if (device_hooks_installed || device == nullptr)
		return;

	DWORD* vtbl = *(DWORD**)device;

	Reset_orig = (Reset_t)vtbl[14];
	auto reset_hook = (DWORD)Reset_hook;
	patch_bytes(&vtbl[14], &reset_hook, sizeof(DWORD));

	Present_orig = (Present_t)vtbl[15];
	auto present_hook = (DWORD)Present_hook;
	patch_bytes(&vtbl[15], &present_hook, sizeof(DWORD));

	device_hooks_installed = true;
}

void D3D8Hook::HookDirect3DCreate8() {
	if (create8_hooked)
		return;

	// The game imports Direct3DCreate8 from d3d8.dll, which may be a wrapper. Hooking the
	// game's import table works for both d3d8to9 and dgVoodoo2, and does nothing for games
	// that don't use D3D8 at all.
	auto orig = iat_hook(NULL, "d3d8.dll", "Direct3DCreate8", (void*)&Direct3DCreate8_hook);
	if (orig) {
		Direct3DCreate8_orig = (Direct3DCreate8_t)orig;
		create8_hooked = true;
	}
}
