#include "d3d8_hook.h"
#include "limiter.h"
#include "patch_util.h"

namespace {
	typedef HRESULT(__stdcall* Present_t)(IDirect3DDevice8*, const RECT*, const RECT*, HWND, const RGNDATA*);

	Present_t Present_orig = nullptr;
	bool installed = false;

	HRESULT __stdcall Present_hook(IDirect3DDevice8* device, const RECT* src_rect, const RECT* dst_rect,
	                               HWND dst_window, const RGNDATA* dirty_region) {
		if (!Limiter::ShouldPresent())
			return D3D_OK;
		return Present_orig(device, src_rect, dst_rect, dst_window, dirty_region);
	}
}

void D3D8Hook::Install(IDirect3DDevice8* device) {
	if (installed || device == nullptr)
		return;

	DWORD* vtbl = *(DWORD**)device;
	Present_orig = (Present_t)vtbl[15];

	auto hook = (DWORD)Present_hook;
	patch_bytes(&vtbl[15], &hook, sizeof(DWORD));

	installed = true;
}
