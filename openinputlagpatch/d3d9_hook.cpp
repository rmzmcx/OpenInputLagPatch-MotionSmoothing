// Hook for D3D9 (and, by extension, D3D8 wrappers) to upgrade to D3D9EX for some latency improvements if enabled
// Also forcefully disables vsync to use a custom frame limiter instead

#include <stdio.h>
#include <d3d9.h>
#include "d3d9_hook.h"
#include "patch_util.h"
#include "common.h"
#include "config.h"
#include "d3d9_overlay.h"
#include "limiter.h"
#include "overlay.h"
#include "present_diag.h"
#include "window_mode.h"

static IDirect3D9* d3d9 = nullptr;
static IDirect3DDevice9* d3d9_device = nullptr;
static D3D9PresentInfo present_info = {};
static UINT native_output_width = 0;
static UINT native_output_height = 0;
static bool using_d3d9ex = false;

static bool loaded_module_is_dgvoodoo2(const wchar_t* module_name);

typedef HRESULT(__stdcall* Present9_t)(IDirect3DDevice9*, const RECT*, const RECT*, HWND,
	const RGNDATA*);

// The device's own Present, taken before anything else can replace it. The frame interpolation
// presents its extra frames with this: they should reach the driver, not the Present hook of the
// user's tool. thprac's hook measures the input latency from the input poll to the next present,
// which only means something when there is one present per game frame - with the extra ones it
// would report a fraction of a frame instead.
static Present9_t present_unhooked = nullptr;

struct D3D9RatioVertex {
	float x;
	float y;
	float z;
	float rhw;
	float u;
	float v;
};
static_assert(sizeof(D3D9RatioVertex) == 24, "D3DFVF_XYZRHW | D3DFVF_TEX1 is 24 bytes");

struct D3D9RatioResources {
	IDirect3DDevice9* device;
	IDirect3DTexture9* texture;
	IDirect3DSurface9* surface;
	UINT width;
	UINT height;
	D3DFORMAT format;
};

static D3D9RatioResources ratio_resources = {};
static bool suppress_end_scene_overlay;
static bool suppress_force_ratio_for_present;

static void capture_native_output_size(HWND window) {
	MONITORINFO monitor = {};
	monitor.cbSize = sizeof(monitor);
	const HMONITOR handle = MonitorFromWindow(window, MONITOR_DEFAULTTOPRIMARY);
	if (handle != nullptr && GetMonitorInfoW(handle, &monitor)) {
		native_output_width = monitor.rcMonitor.right - monitor.rcMonitor.left;
		native_output_height = monitor.rcMonitor.bottom - monitor.rcMonitor.top;
	}
}

static void release_force_ratio_resources() {
	if (ratio_resources.surface != nullptr)
		ratio_resources.surface->Release();
	if (ratio_resources.texture != nullptr)
		ratio_resources.texture->Release();
	ratio_resources = {};
}

static bool ensure_force_ratio_resources(
	IDirect3DDevice9* device, UINT width, UINT height, D3DFORMAT format) {
	if (ratio_resources.device == device && ratio_resources.texture != nullptr &&
		ratio_resources.surface != nullptr && ratio_resources.width == width &&
		ratio_resources.height == height && ratio_resources.format == format) {
		return true;
	}

	release_force_ratio_resources();
	if (FAILED(device->CreateTexture(width, height, 1, D3DUSAGE_RENDERTARGET, format,
		D3DPOOL_DEFAULT, &ratio_resources.texture, nullptr))) {
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

static bool render_force_ratio_4_3(IDirect3DDevice9* device) {
	if (!Config::ForceRatio43 || device == nullptr || present_info.windowed)
		return false;

	IDirect3DSurface9* back_buffer = nullptr;
	if (FAILED(device->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &back_buffer)) ||
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
	if (SUCCEEDED(device->GetDisplayMode(0, &display_mode)) &&
		display_mode.Width != 0 && display_mode.Height != 0) {
		output_width = display_mode.Width;
		output_height = display_mode.Height;
	}
	// Some drivers expose a 4:3 exclusive-fullscreen mode and stretch it to the panel.
	// In that case D3D has no wider mode to report, so use the monitor size captured
	// before the game changed display modes.
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
		D3DVIEWPORT9 viewport = {};
		device->GetViewport(&viewport);
		printf("Force 4:3: D3D9 back buffer %ux%u, output %ux%u, viewport %ux%u, "
			"draw width %.1f, applicable %s\n",
			desc.Width, desc.Height, output_width, output_height,
			viewport.Width, viewport.Height, draw_width,
			output_aspect > 4.0 / 3.0 ? "yes" : "no");
	}
	if (output_aspect <= 4.0 / 3.0) {
		back_buffer->Release();
		return false;
	}
	// In exclusive fullscreen the back buffer can still be 4:3 while the display mode is
	// widescreen. Pre-compress the captured frame by the output aspect so the display's
	// non-uniform stretch restores it to 4:3 with black side bars.
	if (!ensure_force_ratio_resources(
		device, desc.Width, desc.Height, desc.Format)) {
		back_buffer->Release();
		return false;
	}

	IDirect3DStateBlock9* state = nullptr;
	if (FAILED(device->CreateStateBlock(D3DSBT_ALL, &state)) || state == nullptr) {
		back_buffer->Release();
		return false;
	}

	D3DVIEWPORT9 old_viewport = {};
	IDirect3DSurface9* old_target = nullptr;
	IDirect3DSurface9* old_depth = nullptr;
	if (FAILED(device->GetViewport(&old_viewport)) ||
		FAILED(device->GetRenderTarget(0, &old_target)) ||
		FAILED(device->GetDepthStencilSurface(&old_depth))) {
		if (old_target != nullptr)
			old_target->Release();
		if (old_depth != nullptr)
			old_depth->Release();
		state->Release();
		back_buffer->Release();
		return false;
	}

	if (FAILED(device->StretchRect(
		back_buffer, nullptr, ratio_resources.surface, nullptr, D3DTEXF_NONE))) {
		if (old_target != nullptr)
			old_target->Release();
		if (old_depth != nullptr)
			old_depth->Release();
		state->Release();
		back_buffer->Release();
		return false;
	}
	const float width = (float)desc.Width;
	const float height = (float)desc.Height;
	const float scaled_width = (float)draw_width;
	const float offset_x = (width - scaled_width) * 0.5f;
	const D3D9RatioVertex vertices[4] = {
		{ offset_x - 0.5f, -0.5f, 0.5f, 1.0f, 0.0f, 0.0f },
		{ offset_x + scaled_width - 0.5f, -0.5f, 0.5f, 1.0f, 1.0f, 0.0f },
		{ offset_x - 0.5f, height - 0.5f, 0.5f, 1.0f, 0.0f, 1.0f },
		{ offset_x + scaled_width - 0.5f, height - 0.5f, 0.5f, 1.0f, 1.0f, 1.0f },
	};

	bool target_changed = SUCCEEDED(device->SetRenderTarget(0, back_buffer));
	if (target_changed)
		target_changed = SUCCEEDED(device->SetDepthStencilSurface(nullptr));
	back_buffer->Release();
	if (target_changed) {
		D3DVIEWPORT9 full_viewport = { 0, 0, desc.Width, desc.Height, 0.0f, 1.0f };
		device->SetViewport(&full_viewport);
		suppress_end_scene_overlay = true;
		if (SUCCEEDED(device->BeginScene())) {
			device->SetRenderState(D3DRS_SCISSORTESTENABLE, FALSE);
			device->Clear(0, nullptr, D3DCLEAR_TARGET, D3DCOLOR_XRGB(0, 0, 0), 1.0f, 0);

			device->SetTexture(0, ratio_resources.texture);
			device->SetPixelShader(nullptr);
			device->SetVertexShader(nullptr);
			device->SetVertexDeclaration(nullptr);
			device->SetFVF(D3DFVF_XYZRHW | D3DFVF_TEX1);
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
			device->SetRenderState(D3DRS_SRGBWRITEENABLE, FALSE);

			device->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_MODULATE);
			device->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
			device->SetTextureStageState(0, D3DTSS_COLORARG2, D3DTA_DIFFUSE);
			device->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_MODULATE);
			device->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_TEXTURE);
			device->SetTextureStageState(0, D3DTSS_ALPHAARG2, D3DTA_DIFFUSE);
			device->SetTextureStageState(0, D3DTSS_TEXTURETRANSFORMFLAGS, D3DTTFF_DISABLE);
			for (DWORD stage = 1; stage < 8; ++stage) {
				device->SetTextureStageState(stage, D3DTSS_COLOROP, D3DTOP_DISABLE);
				device->SetTextureStageState(stage, D3DTSS_ALPHAOP, D3DTOP_DISABLE);
			}
			device->SetSamplerState(0, D3DSAMP_MINFILTER, D3DTEXF_POINT);
			device->SetSamplerState(0, D3DSAMP_MAGFILTER, D3DTEXF_POINT);
			device->DrawPrimitiveUP(
				D3DPT_TRIANGLESTRIP, 2, vertices, sizeof(D3D9RatioVertex));
			device->EndScene();
		}
		suppress_end_scene_overlay = false;
	}

	state->Apply();
	device->SetViewport(&old_viewport);
	device->SetRenderTarget(0, old_target);
	device->SetDepthStencilSurface(old_depth);
	if (old_target != nullptr)
		old_target->Release();
	if (old_depth != nullptr)
		old_depth->Release();
	state->Release();
	return target_changed;
}

static HRESULT __stdcall Present9_hook(IDirect3DDevice9* device, const RECT* source_rect,
	const RECT* dest_rect, HWND dest_window, const RGNDATA* dirty_region) {
	if (!suppress_force_ratio_for_present)
		render_force_ratio_4_3(device);
	return present_unhooked(device, source_rect, dest_rect, dest_window, dirty_region);
}

HRESULT d3d9_present_frame(IDirect3DDevice9* device) {
	if (device == nullptr)
		return E_FAIL;
	const bool previous_suppression = suppress_force_ratio_for_present;
	if (!previous_suppression)
		render_force_ratio_4_3(device);
	suppress_force_ratio_for_present = true;
	const HRESULT result = device->Present(nullptr, nullptr, nullptr, nullptr);
	suppress_force_ratio_for_present = previous_suppression;
	return result;
}

// The device the patch hooked, or null while the game doesn't have one
IDirect3DDevice9* d3d9_hooked_device() {
	return d3d9_device;
}

bool d3d9ex_device_in_use() {
	return using_d3d9ex;
}

// Presents without going through the hooks other patches and tools put on the device (see above)
HRESULT d3d9_present_bypassing_hooks() {
	if (present_unhooked == nullptr || d3d9_device == nullptr)
		return E_FAIL;
	LARGE_INTEGER present_start;
	LARGE_INTEGER present_end;
	QueryPerformanceCounter(&present_start);
	render_force_ratio_4_3(d3d9_device);
	const HRESULT result =
		present_unhooked(d3d9_device, nullptr, nullptr, nullptr, nullptr);
	QueryPerformanceCounter(&present_end);
	present_diag_note_present_duration((uint64_t)present_start.QuadPart,
	                                   (uint64_t)present_end.QuadPart);
	return result;
}

// Whether a D3D9 device exists, i.e. whether the D3D9 overlay is the one drawing (overlay.h)
bool d3d9_overlay_available() {
	return d3d9_device != nullptr;
}

D3D9PresentInfo d3d9_get_present_info() {
	return present_info;
}

static void capture_present_info(const D3DPRESENT_PARAMETERS* params) {
	present_info.width = params->BackBufferWidth;
	present_info.height = params->BackBufferHeight;
	present_info.back_buffer_count = params->BackBufferCount;
	present_info.swap_effect = params->SwapEffect;
	present_info.presentation_interval = params->PresentationInterval;
	present_info.refresh_rate = params->FullScreen_RefreshRateInHz;
	present_info.windowed = params->Windowed != FALSE;
	present_info.d3d9ex = using_d3d9ex;
}

// Replaces D3DPOOL_MANAGED with D3DPOOL_DEFAULT because it's deprecated on D3D9Ex
auto CreateTexture_orig = (HRESULT(__stdcall*)(IDirect3DDevice9*, UINT, UINT, UINT, DWORD, D3DFORMAT, D3DPOOL, IDirect3DTexture9**, HANDLE*))nullptr;
HRESULT __stdcall CreateTexture_hook(IDirect3DDevice9* self, UINT Width, UINT Height, UINT Levels, DWORD Usage, D3DFORMAT Format,
	D3DPOOL Pool, IDirect3DTexture9** ppTexture, HANDLE* pSharedHandle)
{
	//printf("CreateTexture intercepted!\n");

	if (Pool == D3DPOOL_MANAGED) {
		Pool = D3DPOOL_DEFAULT;
		Usage |= D3DUSAGE_DYNAMIC;
	}
	return CreateTexture_orig(self, Width, Height, Levels, Usage, Format, Pool, ppTexture, pSharedHandle);
}

// Same as CreateTexture_hook
auto CreateVertexBuffer_orig = (HRESULT(__stdcall*)(IDirect3DDevice9*, UINT, DWORD, DWORD, D3DPOOL, IDirect3DVertexBuffer9**, HANDLE*))nullptr;
HRESULT __stdcall CreateVertexBuffer_hook(IDirect3DDevice9* self, UINT Length, DWORD Usage, DWORD FVF, D3DPOOL Pool,
	IDirect3DVertexBuffer9** ppVertexBuffer, HANDLE* pSharedHandle)
{
	printf("CreateVertexBuffer intercepted!\n");

	if (Pool == D3DPOOL_MANAGED) {
		Pool = D3DPOOL_DEFAULT;
		Usage |= D3DUSAGE_DYNAMIC;
	}
	return CreateVertexBuffer_orig(self, Length, Usage, FVF, Pool, ppVertexBuffer, pSharedHandle);
}

// Gets the target fullscreen refresh rate
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
				// Enumerate all supported resolutions and refresh rates
				// TODO: Probably shouldn't always use D3DADAPTER_DEFAULT, but the games probably do that too
				auto count = d3d9->GetAdapterModeCount(D3DADAPTER_DEFAULT, D3DFMT_X8R8G8B8);
				D3DDISPLAYMODE* modes = new D3DDISPLAYMODE[count];
				printf("Supported display modes:\n");
				for (UINT i = 0; i < count; i++) {
					d3d9->EnumAdapterModes(D3DADAPTER_DEFAULT, D3DFMT_X8R8G8B8, i, &modes[i]);
					printf("%d x %d @ %dhz\n", modes[i].Width, modes[i].Height, modes[i].RefreshRate);
				}

				// Pick the highest multiple of 60 refresh rate at the selected resolution
				for (UINT i = 0; i < count; i++) {
					if (modes[i].Width == present_params->BackBufferWidth && modes[i].Height == present_params->BackBufferHeight && (modes[i].RefreshRate == 59 || modes[i].RefreshRate % 60 == 0)) {
						auto rate = modes[i].RefreshRate == 59 ? 60 : modes[i].RefreshRate;
						if (rate > max_refresh_rate)
							max_refresh_rate = rate;
					}
				}
				if (max_refresh_rate == 0) {
					// Nothing suitable at this resolution - let the runtime pick instead of
					// killing the game (this is what the D3D8 path does as well)
					printf("No multiple of 60hz available at that resolution, letting the runtime pick\n");
					return D3DPRESENT_RATE_DEFAULT;
				}

				printf("Picked %dhz\n", max_refresh_rate);
				delete[] modes;
			}
			return max_refresh_rate;
		}
		default:
			panic_msgbox(UiMessage_InvalidRefreshRate, Config::FullscreenRefreshRate);
	}
}

// Modifies presentation parameters to work properly with Direct3D9Ex
void modify_presentation_parameters(D3DPRESENT_PARAMETERS* params) {
	// Window mode: force the mode that was picked on boot, if the question is enabled
	switch (WindowMode::GetEffectiveOverride()) {
	case WindowMode::Override::Windowed:
		params->Windowed = TRUE;
		break;
	case WindowMode::Override::Fullscreen:
		params->Windowed = FALSE;
		break;
	}

	params->PresentationInterval = D3DPRESENT_INTERVAL_IMMEDIATE;
	params->FullScreen_RefreshRateInHz = params->Windowed ? D3DPRESENT_RATE_DEFAULT : get_target_refresh_rate(params);
	params->SwapEffect = D3DSWAPEFFECT_DISCARD;
	params->BackBufferCount = 0;
}

// Gets the display mode for fullscreen
void get_fullscreen_display_mode(D3DPRESENT_PARAMETERS* present_params, D3DDISPLAYMODEEX* mode) {
	mode->Size = sizeof(D3DDISPLAYMODEEX);
	mode->Width = present_params->BackBufferWidth;
	mode->Height = present_params->BackBufferHeight;
	mode->RefreshRate = get_target_refresh_rate(present_params);
	mode->Format = present_params->BackBufferFormat;
	mode->ScanLineOrdering = D3DSCANLINEORDERING_PROGRESSIVE;
}

// Same as CreateDevice_hook but for Reset
auto Reset_orig = (HRESULT(__stdcall*)(IDirect3DDevice9*, D3DPRESENT_PARAMETERS*))nullptr;
HRESULT __stdcall Reset_hook(IDirect3DDevice9* self, D3DPRESENT_PARAMETERS* pPresentationParameters) {
	printf("Reset intercepted!\n");
	modify_presentation_parameters(pPresentationParameters);
	capture_present_info(pPresentationParameters);
	release_force_ratio_resources();

	HRESULT ret;

	if (Config::ShowOverlay && D3D9Overlay::Instance) {
		delete D3D9Overlay::Instance;
		// Null it out so the EndScene hook can't draw with a destroyed overlay if the reset
		// below fails (and so it isn't recreated with stale sizes if it succeeds)
		D3D9Overlay::Instance = nullptr;
	}

	if (using_d3d9ex) {
		D3DDISPLAYMODEEX display_mode = {};
		get_fullscreen_display_mode(pPresentationParameters, &display_mode);

		ret = ((IDirect3DDevice9Ex*)self)->ResetEx(pPresentationParameters, pPresentationParameters->Windowed ? NULL : &display_mode);
	} else {
		// We overwrote thcrap_tsa's hook that fixes device loss errors on D3D9, so its functionality is replicated here
		// This shouldn't apply to D3D9Ex because device loss behaves differently on it
		ret = self->TestCooperativeLevel();
		if (ret != D3D_OK) {
			while (ret != D3D_OK) {
				ret = self->TestCooperativeLevel();
				switch (ret) {
				case D3DERR_DEVICELOST:
					// Can't regain the device right now, check back periodically
					Sleep(16);
					break;
				case D3DERR_DEVICENOTRESET:
					// Device can be reset!
					ret = Reset_orig(self, pPresentationParameters);
					if (FAILED(ret))
						panic_msgbox(UiMessage_DeviceResetFailed, ret);
					break;
				case D3D_OK:
					// Shouldn't be possible but should be gracefully handled anyway
					break;
				default:
					// Something definitely went wrong
					panic_msgbox(UiMessage_TestCooperativeLevelFailed, ret);
				}
			}
		} else {
			ret = Reset_orig(self, pPresentationParameters);
		}
	}

	printf("Reset returned 0x%lx\n", ret);

	// Window mode: the game may have switched between fullscreen and window mode, so the
	// window has to be fixed up again
	if (SUCCEEDED(ret)) {
		D3DDEVICE_CREATION_PARAMETERS creation_params = {};
		self->GetCreationParameters(&creation_params);
		WindowMode::Apply(
			pPresentationParameters->hDeviceWindow ? pPresentationParameters->hDeviceWindow : creation_params.hFocusWindow,
			pPresentationParameters->Windowed
		);
	}

	if (Config::ShowOverlay && SUCCEEDED(ret)) {
		D3D9Overlay::Instance = new D3D9Overlay(d3d9_device, pPresentationParameters->BackBufferWidth, pPresentationParameters->BackBufferHeight);
	}

	return ret;
}

// Renders the overlay if enabled
auto EndScene_orig = (HRESULT(__stdcall*)(IDirect3DDevice9*))nullptr;
HRESULT __stdcall EndScene_hook(IDirect3DDevice9* self) {
	if (!suppress_end_scene_overlay && D3D9Overlay::Instance)
		D3D9Overlay::Instance->Draw();
	return EndScene_orig(self);
}

// Hooks certain D3D9 device functions for better compatibility with D3D9Ex and to force disable vsync
bool first_device_creation = true;
auto CreateDevice_orig = (HRESULT(__stdcall*)(IDirect3D9*, UINT, D3DDEVTYPE, HWND, DWORD, D3DPRESENT_PARAMETERS*, IDirect3DDevice9**))nullptr;
HRESULT __stdcall CreateDevice_hook(IDirect3D9* self, UINT Adapter, D3DDEVTYPE DeviceType, HWND hFocusWindow, DWORD BehaviorFlags,
	D3DPRESENT_PARAMETERS* pPresentationParameters, IDirect3DDevice9** ppReturnedDeviceInterface)
{
	printf("CreateDevice intercepted! DeviceType: %d\n", DeviceType);
	release_force_ratio_resources();
	capture_native_output_size(hFocusWindow ? hFocusWindow : pPresentationParameters->hDeviceWindow);

	// Disable vsync on the parameters
	modify_presentation_parameters(pPresentationParameters);
	capture_present_info(pPresentationParameters);
	
	// Create the device
	IDirect3DDevice9* device = nullptr;
	HRESULT res = 0;

	if (using_d3d9ex) {
		auto self_ex = (IDirect3D9Ex*)self;
		if (pPresentationParameters->Windowed == FALSE) {
			D3DDISPLAYMODEEX display_mode = {};
			get_fullscreen_display_mode(pPresentationParameters, &display_mode);

			res = self_ex->CreateDeviceEx(Adapter, DeviceType, hFocusWindow, BehaviorFlags, pPresentationParameters, &display_mode, (IDirect3DDevice9Ex**)&device);
		}
		else {
			res = self_ex->CreateDeviceEx(Adapter, DeviceType, hFocusWindow, BehaviorFlags, pPresentationParameters, NULL, (IDirect3DDevice9Ex**)&device);
		}
		if (FAILED(res))
			panic_msgbox(UiMessage_CreateDeviceExFailed, res);

		auto device_ex = (IDirect3DDevice9Ex*)device;

		// D3D9Ex breaks the window style for some reason
		SetWindowPos(hFocusWindow, 0, 0, 0, 0, 0, SWP_NOSIZE | SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_SHOWWINDOW);

		// Set the maximum amount of frames that can be queued to 1 instead of 3
		// Should shave off 0 to 2 frames of lag
		device_ex->SetMaximumFrameLatency(1);

		// Set the GPU thread to high priority
		device_ex->SetGPUThreadPriority(7);
	} else {
		res = CreateDevice_orig(self, Adapter, DeviceType, hFocusWindow, BehaviorFlags, pPresentationParameters, &device);
		if (FAILED(res))
			panic_msgbox(UiMessage_CreateDeviceFailed, res);
	}

	*ppReturnedDeviceInterface = device;
	present_diag_set_window(hFocusWindow ? hFocusWindow : pPresentationParameters->hDeviceWindow);

	// Set the CPU thread to high priority
	SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);

	// Overwrite some vtable entries in IDirect3DDevice9
	DWORD* device_vtbl = *(DWORD**)device;

	// Take the device's own Present before the game (and the tools it loads) get their hands on
	// the device. This is what the frame interpolation presents its extra frames with (see above)
	if (device_vtbl[17] != (DWORD)Present9_hook)
		present_unhooked = (Present9_t)device_vtbl[17];
	auto present_hook = (DWORD)Present9_hook;
	patch_bytes(&device_vtbl[17], &present_hook, sizeof(DWORD));

	Reset_orig = (HRESULT(__stdcall*)(IDirect3DDevice9*, D3DPRESENT_PARAMETERS*))device_vtbl[16];
	auto patch_data = (DWORD)Reset_hook;
	patch_bytes(&device_vtbl[16], &patch_data, sizeof(DWORD));

	if (using_d3d9ex) {
		CreateTexture_orig = (HRESULT(__stdcall*)(IDirect3DDevice9*, UINT, UINT, UINT, DWORD, D3DFORMAT, D3DPOOL, IDirect3DTexture9**, HANDLE*))device_vtbl[23];
		patch_data = (DWORD)CreateTexture_hook;
		patch_bytes(&device_vtbl[23], &patch_data, sizeof(DWORD));

		CreateVertexBuffer_orig = (HRESULT(__stdcall*)(IDirect3DDevice9*, UINT, DWORD, DWORD, D3DPOOL, IDirect3DVertexBuffer9**, HANDLE*))device_vtbl[26];
		patch_data = (DWORD)CreateVertexBuffer_hook;
		patch_bytes(&device_vtbl[26], &patch_data, sizeof(DWORD));
	}

	if (Config::ShowOverlay) {
		EndScene_orig = (HRESULT(__stdcall*)(IDirect3DDevice9*))device_vtbl[42];
		patch_data = (DWORD)EndScene_hook;
		patch_bytes(&device_vtbl[42], &patch_data, sizeof(DWORD));
	}

	// (Re)initialize the overlay
	if (D3D9Overlay::Instance)
		delete D3D9Overlay::Instance;
	D3D9Overlay::Instance = new D3D9Overlay(device, pPresentationParameters->BackBufferWidth, pPresentationParameters->BackBufferHeight);
	d3d9_device = device;

	// Window mode: the game's window already exists at this point, so it can be adjusted
	WindowMode::Apply(
		hFocusWindow ? hFocusWindow : pPresentationParameters->hDeviceWindow,
		pPresentationParameters->Windowed
	);

	return 0;
}

// The game's folder can have a d3d9.dll of its own that doesn't export everything (Ju.N.Owen's on th19 only exports
// Direct3DCreate9), and importing from it would make the patch fail to load. So these are looked up at runtime instead:
// from the d3d9.dll the game uses if it has them, otherwise from the system's d3d9.dll
static FARPROC get_system_d3d9_function(const char* name) {
	wchar_t path[MAX_PATH] = {};
	GetSystemDirectoryW(path, MAX_PATH);
	wcscat_s(path, L"\\d3d9.dll");
	auto system_d3d9 = LoadLibraryW(path);
	return system_d3d9 ? GetProcAddress(system_d3d9, name) : nullptr;
}

static FARPROC get_d3d9_function(const char* name) {
	auto loaded = GetModuleHandleW(L"d3d9.dll");
	if (loaded) {
		auto func = GetProcAddress(loaded, name);
		if (func)
			return func;
	}
	return get_system_d3d9_function(name);
}

// What the system's Direct3DCreate9 returns while wrap_with_d3d9_proxy has it redirected
static IDirect3D9* proxy_inner = nullptr;
static IDirect3D9* WINAPI Direct3DCreate9_inner(UINT SDKVersion) {
	return proxy_inner;
}

// A d3d9.dll without Direct3DCreate9Ex in the game's folder (Ju.N.Owen's on th19) only gets to run its own code
// from its Direct3DCreate9: Ju.N.Owen loads its modules there, and the current one wraps the IDirect3D9 the
// system's Direct3DCreate9 gives it, to draw its lobby right before every Present. So it's still called, with
// the system's Direct3DCreate9 redirected for the duration of the call to hand it the D3D9Ex object instead -
// that way it wraps the object the patch created, and the game gets the wrapper like it would without the patch.
// Returns the D3D9Ex object itself when there's no such d3d9.dll or it doesn't return anything
static IDirect3D9* wrap_with_d3d9_proxy(IDirect3D9* d3d9_ex, UINT SDKVersion) {
	auto loaded = GetModuleHandleW(L"d3d9.dll");
	if (!loaded || GetProcAddress(loaded, "Direct3DCreate9Ex"))
		return d3d9_ex;
	auto proxy_create = (decltype(&Direct3DCreate9))GetProcAddress(loaded, "Direct3DCreate9");
	auto system_create = (BYTE*)get_system_d3d9_function("Direct3DCreate9");
	if (!proxy_create || !system_create)
		return d3d9_ex;
	printf("d3d9.dll without Direct3DCreate9Ex detected, letting its Direct3DCreate9 wrap the D3D9Ex object\n");

	// The proxy takes over the reference it's given, so it gets one of its own
	d3d9_ex->AddRef();
	proxy_inner = d3d9_ex;

	// jmp Direct3DCreate9_inner
	BYTE orig[5];
	memcpy(orig, system_create, sizeof(orig));
	BYTE jump[5] = { 0xE9 };
	*(DWORD*)(jump + 1) = (DWORD)&Direct3DCreate9_inner - ((DWORD)system_create + 5);
	patch_bytes(system_create, jump, sizeof(jump));
	FlushInstructionCache(GetCurrentProcess(), system_create, sizeof(jump));

	auto wrapped = proxy_create(SDKVersion);

	patch_bytes(system_create, orig, sizeof(orig));
	FlushInstructionCache(GetCurrentProcess(), system_create, sizeof(orig));
	proxy_inner = nullptr;

	return wrapped ? wrapped : d3d9_ex;
}

// Upgrades IDirect3D9 to IDirect3D9Ex
IDirect3D9* WINAPI Direct3DCreate9_hook(UINT SDKVersion) {
	printf("Direct3DCreate9 intercepted!\n");

	// Window mode: only a fallback for the games that get the window mode question - those
	// are all D3D8 titles, so this normally does nothing
	WindowMode::AskIfNeeded();

	const bool request_d3d9ex = Config::D3D9Ex != FALSE;
	const bool dgvoodoo2 = loaded_module_is_dgvoodoo2(L"d3d9.dll");
	using_d3d9ex = request_d3d9ex && !dgvoodoo2;
	if (request_d3d9ex && dgvoodoo2) {
		printf("dgVoodoo2 detected - skipping the Direct3D9Ex upgrade to preserve "
			"exclusive-fullscreen aspect ratio after Reset.\n");
	}

	IDirect3D9* result = nullptr;
	if (using_d3d9ex) {
		auto create_ex = (decltype(&Direct3DCreate9Ex))get_d3d9_function("Direct3DCreate9Ex");
		if (!create_ex)
			panic_msgbox(UiMessage_Direct3DCreate9ExNotFound);
		auto create_ret = create_ex(D3D_SDK_VERSION, (IDirect3D9Ex**)&d3d9);
		if (FAILED(create_ret) || d3d9 == nullptr)
			panic_msgbox(UiMessage_Direct3DCreate9ExFailed, create_ret);
		result = wrap_with_d3d9_proxy(d3d9, SDKVersion);
	} else {
		auto create = (decltype(&Direct3DCreate9))get_d3d9_function("Direct3DCreate9");
		if (!create)
			panic_msgbox(UiMessage_Direct3DCreate9NotFound);
		d3d9 = create(D3D_SDK_VERSION);
		if (d3d9 == nullptr)
			panic_msgbox(UiMessage_Direct3DCreate9Failed);
		result = d3d9;
	}

	// Hook IDirect3D9::CreateDevice
	// A wrapper from wrap_with_d3d9_proxy forwards CreateDevice to the D3D9Ex object, so the hook goes on that
	DWORD* d3d9_vtbl = *(DWORD**)d3d9;
	CreateDevice_orig = (HRESULT(__stdcall*)(IDirect3D9*, UINT, D3DDEVTYPE, HWND, DWORD, D3DPRESENT_PARAMETERS*, IDirect3DDevice9**))d3d9_vtbl[16];
	auto patch_data = (DWORD)CreateDevice_hook;
	patch_bytes(&d3d9_vtbl[16], &patch_data, sizeof(DWORD));
	return result;
}

// Case-insensitive substring search (no extra includes needed)
static bool contains_ci(const wchar_t* haystack, const wchar_t* needle) {
	if (needle[0] == L'\0')
		return true;
	for (size_t start = 0; haystack[start] != L'\0'; start++) {
		size_t i = 0;
		for (; needle[i] != L'\0'; i++) {
			wchar_t a = haystack[start + i];
			wchar_t b = needle[i];
			if (a == L'\0')
				return false;
			if (a >= L'A' && a <= L'Z')
				a = (wchar_t)(a - L'A' + L'a');
			if (b >= L'A' && b <= L'Z')
				b = (wchar_t)(b - L'A' + L'a');
			if (a != b)
				break;
		}
		if (needle[i] == L'\0')
			return true;
	}
	return false;
}

// True only if the named loaded module really is dgVoodoo2.
// We identify it by its version resource: dgVoodoo2's D3D8/D3D9 DLLs report
// ProductName "dgVoodoo" and a FileDescription that names the wrapped API.
// This is a precise check on purpose - an unknown wrapper should still get the warning.
static bool loaded_module_is_dgvoodoo2(const wchar_t* module_name) {
	HMODULE mod = GetModuleHandleW(module_name);
	if (mod == NULL)
		return false;

	wchar_t path[MAX_PATH] = {};
	if (GetModuleFileNameW(mod, path, MAX_PATH) == 0)
		return false;

	DWORD handle = 0;
	DWORD size = GetFileVersionInfoSizeW(path, &handle);
	if (size == 0 || size > 4096)
		return false;

	BYTE buffer[4096];
	if (!GetFileVersionInfoW(path, 0, size, buffer))
		return false;

	// Walk every language/codepage block and look at ProductName
	struct LangAndCodePage {
		WORD language;
		WORD codePage;
	} *translations = NULL;
	UINT translations_size = 0;
	if (!VerQueryValueW(buffer, L"\\VarFileInfo\\Translation", (LPVOID*)&translations, &translations_size))
		return false;
	if (translations == NULL || translations_size < sizeof(LangAndCodePage))
		return false;

	const UINT count = translations_size / sizeof(LangAndCodePage);
	for (UINT i = 0; i < count; i++) {
		wchar_t sub_block[128];
		wsprintfW(sub_block, L"\\StringFileInfo\\%04x%04x\\ProductName", translations[i].language,
		          translations[i].codePage);

		wchar_t* product_name = NULL;
		UINT product_name_size = 0;
		if (VerQueryValueW(buffer, sub_block, (LPVOID*)&product_name, &product_name_size) && product_name != NULL &&
		    product_name_size > 0) {
			if (contains_ci(product_name, L"dgVoodoo"))
				return true;
		}
	}
	return false;
}

// Intercepts Direct3DCreate9 to Direct3DCreate9Ex
void hook_d3d9() {
	// Try to IAT hook Direct3DCreate9 from the main executable
	if (!iat_hook(NULL, "d3d9.dll", "Direct3DCreate9", (void*)&Direct3DCreate9_hook)) {
		// Try to IAT hook the D3D8 wrapper
		if (!iat_hook(L"d3d8.dll", "d3d9.dll", "Direct3DCreate9", (void*)&Direct3DCreate9_hook)) {
			// dgVoodoo2 translates D3D8 to D3D11 instead of D3D9, so there is no
			// Direct3DCreate9 import to hook. It is a known-good wrapper though, so we only
			// skip the D3D9Ex upgrade quietly instead of warning the user.
			if (loaded_module_is_dgvoodoo2(L"d3d8.dll")) {
				printf("dgVoodoo2 detected - it doesn't go through d3d9.dll. Skipping D3D9Ex upgrade.\n");
			} else {
				// No D3D9 to hook at all. That only costs the D3D9Ex upgrade (the extra input
				// lag reduction), so this is a console message instead of a popup: it's useful
				// when debugging but not worth interrupting every launch for.
				printf(
					"Couldn't hook Direct3DCreate9: no D3D8 wrapper (dgVoodoo2 or d3d8to9) was found, "
					"or the one in use doesn't go through D3D9.\n"
					"The Direct3D9Ex input lag reduction isn't available without a D3D9 device - "
					"install dgVoodoo2 or d3d8to9 for the lowest input lag.\n"
				);
			}
		}
	}
}
