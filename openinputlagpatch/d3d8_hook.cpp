#include <stdio.h>
#include <intrin.h>

#include "d3d8_hook.h"
#include "d3d8/d3d8.h"
#include "config.h"
#include "d3d8_overlay.h"
#include "limiter.h"
#include "overlay.h"
#include "patch_util.h"
#include "present_diag.h"
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
	IDirect3D8* d3d8_interface = nullptr;
	IDirect3DDevice8* d3d8_device = nullptr;
	bool device_hooks_installed = false;
	bool create8_hooked = false;
	D3D8Hook::DeviceResetCallback device_reset_callback = nullptr;
	D3D8Hook::Direct3DCreateCallback direct3d_create_callback = nullptr;
	D3D8Hook::PresentInfo present_info = {};
	UINT native_output_width = 0;
	UINT native_output_height = 0;

	void capture_native_output_size(HWND window) {
		MONITORINFO monitor = {};
		monitor.cbSize = sizeof(monitor);
		const HMONITOR handle = MonitorFromWindow(window, MONITOR_DEFAULTTOPRIMARY);
		if (handle != nullptr && GetMonitorInfoW(handle, &monitor)) {
			native_output_width = monitor.rcMonitor.right - monitor.rcMonitor.left;
			native_output_height = monitor.rcMonitor.bottom - monitor.rcMonitor.top;
		}
	}

	struct D3D8RatioVertex {
		float x;
		float y;
		float z;
		float rhw;
		float u;
		float v;
	};
	static_assert(sizeof(D3D8RatioVertex) == 24, "D3DFVF_XYZRHW | D3DFVF_TEX1 is 24 bytes");

	struct D3D8RatioResources {
		IDirect3DDevice8* device;
		IDirect3DTexture8* texture;
		IDirect3DSurface8* surface;
		UINT width;
		UINT height;
		D3DFORMAT format;
	};

	D3D8RatioResources ratio_resources = {};

	void release_force_ratio_resources() {
		if (ratio_resources.surface != nullptr)
			ratio_resources.surface->Release();
		if (ratio_resources.texture != nullptr)
			ratio_resources.texture->Release();
		ratio_resources = {};
	}

	bool ensure_force_ratio_resources(
		IDirect3DDevice8* device, UINT width, UINT height, D3DFORMAT format) {
		if (ratio_resources.device == device && ratio_resources.texture != nullptr &&
			ratio_resources.surface != nullptr && ratio_resources.width == width &&
			ratio_resources.height == height && ratio_resources.format == format) {
			return true;
		}

		release_force_ratio_resources();
		if (FAILED(device->CreateTexture(width, height, 1, D3DUSAGE_RENDERTARGET,
			format, D3DPOOL_DEFAULT, &ratio_resources.texture))) {
			ratio_resources.texture = nullptr;
			return false;
		}
		if (FAILED(ratio_resources.texture->GetSurfaceLevel(0, &ratio_resources.surface))) {
			ratio_resources.texture->Release();
			ratio_resources = {};
			return false;
		}
		ratio_resources.device = device;
		ratio_resources.width = width;
		ratio_resources.height = height;
		ratio_resources.format = format;
		return true;
	}

	bool render_force_ratio_4_3(IDirect3DDevice8* device) {
		if (!Config::ForceRatio43 || device == nullptr || present_info.windowed)
			return false;

		IDirect3DSurface8* back_buffer = nullptr;
		if (FAILED(device->GetBackBuffer(0, D3DBACKBUFFER_TYPE_MONO, &back_buffer)) ||
			back_buffer == nullptr) {
			return false;
		}

		D3DSURFACE_DESC desc = {};
		if (FAILED(back_buffer->GetDesc(&desc))) {
			back_buffer->Release();
			return false;
		}

		UINT output_width = desc.Width;
		UINT output_height = desc.Height;
		D3DDISPLAYMODE display_mode = {};
		if (SUCCEEDED(device->GetDisplayMode(&display_mode)) &&
			display_mode.Width != 0 && display_mode.Height != 0) {
			output_width = display_mode.Width;
			output_height = display_mode.Height;
		}
		if ((uint64_t)output_width * 3 <= (uint64_t)output_height * 4 &&
			(uint64_t)native_output_width * 3 > (uint64_t)native_output_height * 4) {
			output_width = native_output_width;
			output_height = native_output_height;
		}
		const double output_aspect = (double)output_width / (double)output_height;
		const double draw_width = (double)desc.Width * (4.0 / 3.0) / output_aspect;
		static bool ratio_logged;
		if (!ratio_logged) {
			ratio_logged = true;
			D3DVIEWPORT8 viewport = {};
			device->GetViewport(&viewport);
			printf("Force 4:3: D3D8 back buffer %ux%u, output %ux%u, viewport %ux%u, "
				"draw width %.1f, applicable %s\n",
				desc.Width, desc.Height, output_width, output_height,
				viewport.Width, viewport.Height, draw_width,
				output_aspect > 4.0 / 3.0 ? "yes" : "no");
		}
		if (output_aspect <= 4.0 / 3.0) {
			back_buffer->Release();
			return false;
		}
		// Exclusive fullscreen can stretch a 4:3 back buffer to a widescreen display mode.
		// Compress the captured frame against the actual output aspect so the stretch puts
		// the image back at 4:3 with black bars.
		if (!ensure_force_ratio_resources(device, desc.Width, desc.Height, desc.Format)) {
			back_buffer->Release();
			return false;
		}

		DWORD state = 0;
		if (FAILED(device->CreateStateBlock(D3DSBT_ALL, &state))) {
			back_buffer->Release();
			return false;
		}

		D3DVIEWPORT8 old_viewport = {};
		IDirect3DSurface8* old_target = nullptr;
		IDirect3DSurface8* old_depth = nullptr;
		if (FAILED(device->GetViewport(&old_viewport)) ||
			FAILED(device->GetRenderTarget(&old_target)) ||
			FAILED(device->GetDepthStencilSurface(&old_depth))) {
			if (old_target != nullptr)
				old_target->Release();
			if (old_depth != nullptr)
				old_depth->Release();
			device->DeleteStateBlock(state);
			back_buffer->Release();
			return false;
		}

		RECT source_rect = { 0, 0, (LONG)desc.Width, (LONG)desc.Height };
		POINT destination = { 0, 0 };
		if (FAILED(device->CopyRects(
			back_buffer, &source_rect, 1, ratio_resources.surface, &destination))) {
			if (old_target != nullptr)
				old_target->Release();
			if (old_depth != nullptr)
				old_depth->Release();
			device->DeleteStateBlock(state);
			back_buffer->Release();
			return false;
		}

		const float width = (float)desc.Width;
		const float height = (float)desc.Height;
		const float scaled_width = (float)draw_width;
		const float offset_x = (width - scaled_width) * 0.5f;
		const D3D8RatioVertex vertices[4] = {
			{ offset_x - 0.5f, -0.5f, 0.5f, 1.0f, 0.0f, 0.0f },
			{ offset_x + scaled_width - 0.5f, -0.5f, 0.5f, 1.0f, 1.0f, 0.0f },
			{ offset_x - 0.5f, height - 0.5f, 0.5f, 1.0f, 0.0f, 1.0f },
			{ offset_x + scaled_width - 0.5f, height - 0.5f, 0.5f, 1.0f, 1.0f, 1.0f },
		};

		bool target_changed = SUCCEEDED(device->SetRenderTarget(back_buffer, nullptr));
		back_buffer->Release();
		if (target_changed) {
			D3DVIEWPORT8 full_viewport = { 0, 0, desc.Width, desc.Height, 0.0f, 1.0f };
			device->SetViewport(&full_viewport);
			if (SUCCEEDED(device->BeginScene())) {
				device->Clear(0, nullptr, D3DCLEAR_TARGET, D3DCOLOR_XRGB(0, 0, 0), 1.0f, 0);

				device->SetVertexShader(D3DFVF_XYZRHW | D3DFVF_TEX1);
				device->SetTexture(0, ratio_resources.texture);
				device->SetPixelShader(0);
				device->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
				device->SetRenderState(D3DRS_LIGHTING, FALSE);
				device->SetRenderState(D3DRS_ZENABLE, D3DZB_FALSE);
				device->SetRenderState(D3DRS_STENCILENABLE, FALSE);
				device->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
				device->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);
				device->SetRenderState(D3DRS_BLENDOP, D3DBLENDOP_ADD);
				device->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_SRCALPHA);
				device->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);
				device->SetRenderState(D3DRS_SHADEMODE, D3DSHADE_GOURAUD);
				device->SetRenderState(D3DRS_FOGENABLE, FALSE);
				device->SetRenderState(D3DRS_COLORWRITEENABLE,
					D3DCOLORWRITEENABLE_RED | D3DCOLORWRITEENABLE_GREEN |
					D3DCOLORWRITEENABLE_BLUE | D3DCOLORWRITEENABLE_ALPHA);

				device->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_MODULATE);
				device->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
				device->SetTextureStageState(0, D3DTSS_COLORARG2, D3DTA_DIFFUSE);
				device->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_MODULATE);
				device->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_TEXTURE);
				device->SetTextureStageState(0, D3DTSS_ALPHAARG2, D3DTA_DIFFUSE);
				device->SetTextureStageState(0, D3DTSS_TEXTURETRANSFORMFLAGS, D3DTTFF_DISABLE);
				device->SetTextureStageState(0, D3DTSS_MINFILTER, D3DTEXF_POINT);
				device->SetTextureStageState(0, D3DTSS_MAGFILTER, D3DTEXF_POINT);
				for (DWORD stage = 1; stage < 8; ++stage) {
					device->SetTextureStageState(stage, D3DTSS_COLOROP, D3DTOP_DISABLE);
					device->SetTextureStageState(stage, D3DTSS_ALPHAOP, D3DTOP_DISABLE);
				}
				device->DrawPrimitiveUP(
					D3DPT_TRIANGLESTRIP, 2, vertices, sizeof(D3D8RatioVertex));
				device->EndScene();
			}
		}

		device->ApplyStateBlock(state);
		device->SetViewport(&old_viewport);
		device->SetRenderTarget(old_target, old_depth);
		if (old_target != nullptr)
			old_target->Release();
		if (old_depth != nullptr)
			old_depth->Release();
		device->DeleteStateBlock(state);
		return target_changed;
	}

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

	// Chooses the refresh rate a fullscreen device should use, the same way the D3D9 hook
	// does it for the D3D9 games. D3D8 games only ever talk to D3D8, so without this the
	// FullscreenRefreshRate option did nothing for them - the game asks for the current
	// (desktop) rate and the display keeps using it no matter what the option says.
	// TODO: This might not be so great on multi-monitor setups
	UINT max_refresh_rate = 0;
	UINT get_target_refresh_rate(D3DPRESENT_PARAMETERS* present_params) {
		switch (Config::FullscreenRefreshRate) {
			case TargetRefreshRate::Max:
				return D3DPRESENT_RATE_DEFAULT;
			case TargetRefreshRate::Sixty:
				return 60;
			case TargetRefreshRate::MultipleOfSixty: {
				if (max_refresh_rate == 0) {
					// Reset can ask to keep the current size by passing 0, in which case the
					// resolution to match comes from the display mode the device is in
					UINT width = present_params->BackBufferWidth;
					UINT height = present_params->BackBufferHeight;
					if (width == 0 || height == 0) {
						D3DDISPLAYMODE current_mode = {};
						if (SUCCEEDED(d3d8_interface->GetAdapterDisplayMode(D3DADAPTER_DEFAULT, &current_mode))) {
							width = current_mode.Width;
							height = current_mode.Height;
						}
					}

					// Enumerate all supported resolutions and refresh rates
					auto count = d3d8_interface->GetAdapterModeCount(D3DADAPTER_DEFAULT);
					printf("Supported display modes:\n");
					for (UINT i = 0; i < count; i++) {
						D3DDISPLAYMODE mode = {};
						if (FAILED(d3d8_interface->EnumAdapterModes(D3DADAPTER_DEFAULT, i, &mode)))
							continue;
						printf("%d x %d @ %dhz\n", mode.Width, mode.Height, mode.RefreshRate);

						// Pick the highest multiple of 60 refresh rate at the selected resolution
						if (mode.Width == width && mode.Height == height &&
							(mode.RefreshRate == 59 || mode.RefreshRate % 60 == 0)) {
							auto rate = mode.RefreshRate == 59 ? 60 : mode.RefreshRate;
							if (rate > max_refresh_rate)
								max_refresh_rate = rate;
						}
					}
					if (max_refresh_rate == 0) {
						// Nothing suitable at this resolution - let the runtime pick instead of
						// killing the game like the D3D9 version does
						printf("No multiple of 60hz available at that resolution, letting the runtime pick\n");
						return D3DPRESENT_RATE_DEFAULT;
					}
					printf("Picked %dhz\n", max_refresh_rate);
				}
				return max_refresh_rate;
			}
		}
		return D3DPRESENT_RATE_DEFAULT;
	}

	void apply_presentation_overrides(D3DPRESENT_PARAMETERS* params) {
		apply_window_override(params);

		// Same as the D3D9 hook: vsync is forced off and the game's own frame limiter paces the
		// game instead. In D3D8 both FullScreen_* fields have to stay zero while windowed, so
		// they can only be set for a fullscreen device (windowed presents always use the
		// desktop's vsync in D3D8, that can't be turned off here).
		if (!params->Windowed) {
			if (d3d8_interface != nullptr)
				params->FullScreen_RefreshRateInHz = get_target_refresh_rate(params);
			params->FullScreen_PresentationInterval = D3DPRESENT_INTERVAL_IMMEDIATE;
		}

		// The swap effect and the back buffer count are left alone, deliberately. The games read
		// back buffer 0 right after presenting - GameWindow::Present calls Present() and then
		// AnmManager::TakeScreencaptures, which does GetBackBuffer(0, D3DBACKBUFFER_TYPE_MONO) and
		// copies that into the surface their afterimage effects (and the pause screen) draw from -
		// and D3DSWAPEFFECT_DISCARD means exactly "the back buffer's contents are undefined after
		// a present". Forcing it (and a back buffer count of 0) made those captures read something
		// other than the frame that was just drawn, which showed up as the afterimage trail in
		// Touhou 8's stage 6A intro looking different. What the game asks for (COPY, or FLIP /
		// COPY_VSYNC with vsync on) is what it expects to read back.
	}

	// The overlay is drawn by the D3D9 hook's EndScene hook whenever there is a D3D9 device. For
	// D3D8 games whose wrapper never goes through D3D9 (dgVoodoo2 translates straight to D3D11)
	// there is none, so those draw it with their D3D8 device instead, right where the game's own
	// frame ends - which is also where the frame interpolation's extra presentations need it.
	void draw_overlay() {
		if (D3D8Overlay::Instance)
			D3D8Overlay::Instance->Draw();
	}

	HRESULT __stdcall Present_hook(IDirect3DDevice8* device, const RECT* src_rect, const RECT* dst_rect,
	                               HWND dst_window, const RGNDATA* dirty_region) {
		if (!Limiter::ShouldPresent())
			return D3D_OK;

		draw_overlay();
		// When the game is translated through D3D9, the final D3D9 Present applies the
		// 4:3 composition instead. Doing it here as well would add the bars twice.
		if (!d3d9_overlay_available())
			render_force_ratio_4_3(device);

		LARGE_INTEGER present_start;
		LARGE_INTEGER present_end;
		QueryPerformanceCounter(&present_start);
		auto result = Present_orig(device, src_rect, dst_rect, dst_window, dirty_region);
		QueryPerformanceCounter(&present_end);
		present_diag_note_present_duration((uint64_t)present_start.QuadPart,
		                                   (uint64_t)present_end.QuadPart);

		// The first completed Present is the first frame safe to expose. th095's startup window is
		// hidden until this point so device/window setup cannot flash an intermediate frame.
		WindowMode::ShowStartupWindow();

		// One presentation reached the display (see overlay.h; the rate readout is only drawn by
		// the games that turn it on)
		overlay_mark_presentation();
		return result;
	}

	HRESULT __stdcall Reset_hook(IDirect3DDevice8* device, D3DPRESENT_PARAMETERS* present_params) {
		printf("D3D8 Reset intercepted!\n");
		void* caller = _ReturnAddress();
		apply_presentation_overrides(present_params);
		present_info.width = present_params->BackBufferWidth;
		present_info.height = present_params->BackBufferHeight;
		present_info.back_buffer_count = present_params->BackBufferCount;
		present_info.swap_effect = present_params->SwapEffect;
		present_info.presentation_interval = present_params->FullScreen_PresentationInterval;
		present_info.windowed = present_params->Windowed != FALSE;
		release_force_ratio_resources();

		// The overlay's resources are all in D3DPOOL_DEFAULT, and D3D8's Reset fails when any
		// of them are still alive (the game releases its own surfaces for exactly this
		// reason). Leaving them around makes the game quit when alt-tabbing back into an
		// exclusive fullscreen game, since it treats a failed Reset as a fatal error.
		// The D3D9 overlay is released before the reset for the same reason.
		bool recreate_overlay = Config::ShowOverlay && !d3d9_overlay_available();
		if (recreate_overlay) {
			delete D3D8Overlay::Instance;
			D3D8Overlay::Instance = nullptr;
		}

		auto ret = Reset_orig(device, present_params);

		if (SUCCEEDED(ret)) {
			D3DDEVICE_CREATION_PARAMETERS creation_params = {};
			device->GetCreationParameters(&creation_params);
			WindowMode::Apply(
				present_params->hDeviceWindow ? present_params->hDeviceWindow : creation_params.hFocusWindow,
				present_params->Windowed
			);

			if (device_reset_callback != nullptr)
				device_reset_callback(device, caller);

			if (recreate_overlay)
				D3D8Overlay::Instance = new D3D8Overlay(device, present_params->BackBufferWidth, present_params->BackBufferHeight);
		}

		return ret;
	}

	HRESULT __stdcall CreateDevice_hook(IDirect3D8* self, UINT Adapter, D3DDEVTYPE DeviceType, HWND hFocusWindow,
	                                    DWORD BehaviorFlags, D3DPRESENT_PARAMETERS* pp, IDirect3DDevice8** ppReturnedDeviceInterface) {
		printf("D3D8 CreateDevice intercepted!\n");
		release_force_ratio_resources();
		capture_native_output_size(hFocusWindow ? hFocusWindow : pp->hDeviceWindow);
		apply_presentation_overrides(pp);
		present_info.width = pp->BackBufferWidth;
		present_info.height = pp->BackBufferHeight;
		present_info.back_buffer_count = pp->BackBufferCount;
		present_info.swap_effect = pp->SwapEffect;
		present_info.presentation_interval = pp->FullScreen_PresentationInterval;
		present_info.windowed = pp->Windowed != FALSE;

		auto ret = CreateDevice_orig(self, Adapter, DeviceType, hFocusWindow, BehaviorFlags, pp, ppReturnedDeviceInterface);

		if (SUCCEEDED(ret) && ppReturnedDeviceInterface != nullptr && *ppReturnedDeviceInterface != nullptr) {
			present_diag_set_window(hFocusWindow ? hFocusWindow : pp->hDeviceWindow);
			D3D8Hook::Install(*ppReturnedDeviceInterface);
			WindowMode::Apply(hFocusWindow ? hFocusWindow : pp->hDeviceWindow, pp->Windowed);

			if (Config::ShowOverlay && !d3d9_overlay_available()) {
				delete D3D8Overlay::Instance;
				D3D8Overlay::Instance = nullptr;
				D3D8Overlay::Instance = new D3D8Overlay(*ppReturnedDeviceInterface, pp->BackBufferWidth, pp->BackBufferHeight);
			}
		}

		return ret;
	}

	IDirect3D8* WINAPI Direct3DCreate8_hook(UINT SDKVersion) {
		printf("Direct3DCreate8 intercepted!\n");

		// Window mode: this runs after the game read its config but before it creates its
		// window, which is the point where the game's own windowed flag has to be set
		WindowMode::OnD3DCreate();
		if (direct3d_create_callback != nullptr)
			direct3d_create_callback();

		auto d3d8 = Direct3DCreate8_orig(SDKVersion);
		if (d3d8 == nullptr)
			return nullptr;

		d3d8_interface = d3d8;

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
	if (device == nullptr)
		return;

	// Remembered before the hooks are installed, so a device that is recreated later still presents
	// through the same path (see d3d8_present_bypassing_hooks)
	d3d8_device = device;

	if (device_hooks_installed)
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

void D3D8Hook::SetDeviceResetCallback(DeviceResetCallback callback) {
	device_reset_callback = callback;
}

void D3D8Hook::SetDirect3DCreateCallback(Direct3DCreateCallback callback) {
	direct3d_create_callback = callback;
}

D3D8Hook::PresentInfo D3D8Hook::GetPresentInfo() {
	return present_info;
}

// Presents without going through the hooks other patches and tools put on the device (see the
// comment in d3d9_hook.cpp for why the frame interpolation wants that). The patch's own overlay is
// drawn here, the same way the Present hook draws it - this path goes around that hook, and the
// overlay has to stay on every presentation.
HRESULT d3d8_present_bypassing_hooks() {
	if (Present_orig == nullptr || d3d8_device == nullptr)
		return E_FAIL;

	draw_overlay();
	if (!d3d9_overlay_available())
		render_force_ratio_4_3(d3d8_device);
	LARGE_INTEGER present_start;
	LARGE_INTEGER present_end;
	QueryPerformanceCounter(&present_start);
	auto result = Present_orig(d3d8_device, nullptr, nullptr, nullptr, nullptr);
	QueryPerformanceCounter(&present_end);
	present_diag_note_present_duration((uint64_t)present_start.QuadPart,
	                                   (uint64_t)present_end.QuadPart);
	overlay_mark_presentation();
	return result;
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
