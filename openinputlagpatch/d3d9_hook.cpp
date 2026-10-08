// Hook for D3D9 (and, by extension, D3D8 wrappers) to upgrade to D3D9EX for some latency improvements if enabled
// Also forcefully disables vsync to use a custom frame limiter instead

#include <stdio.h>
#include <d3d9.h>
#include "patch_util.h"
#include "common.h"
#include "config.h"
#include "d3d9_overlay.h"
#include "limiter.h"
#include "overlay.h"
#include "window_mode.h"

static IDirect3D9* d3d9 = nullptr;
static IDirect3DDevice9* d3d9_device = nullptr;

// The device's own Present, taken before anything else can replace it. The frame interpolation
// presents its extra frames with this: they should reach the driver, not the Present hook of the
// user's tool. thprac's hook measures the input latency from the input poll to the next present,
// which only means something when there is one present per game frame - with the extra ones it
// would report a fraction of a frame instead.
static HRESULT(__stdcall* present_unhooked)(IDirect3DDevice9*, const RECT*, const RECT*, HWND, const RGNDATA*) = nullptr;

// The device the patch hooked, or null while the game doesn't have one
IDirect3DDevice9* d3d9_hooked_device() {
	return d3d9_device;
}

// Presents without going through the hooks other patches and tools put on the device (see above)
HRESULT d3d9_present_bypassing_hooks() {
	if (present_unhooked == nullptr || d3d9_device == nullptr)
		return E_FAIL;
	return present_unhooked(d3d9_device, nullptr, nullptr, nullptr, nullptr);
}

// Whether a D3D9 device exists, i.e. whether the D3D9 overlay is the one drawing (overlay.h)
bool d3d9_overlay_available() {
	return d3d9_device != nullptr;
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
			panic_msgbox(L"Invalid target refresh rate %d", Config::FullscreenRefreshRate);
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

	HRESULT ret;

	if (Config::ShowOverlay && D3D9Overlay::Instance) {
		delete D3D9Overlay::Instance;
		// Null it out so the EndScene hook can't draw with a destroyed overlay if the reset
		// below fails (and so it isn't recreated with stale sizes if it succeeds)
		D3D9Overlay::Instance = nullptr;
	}

	if (Config::D3D9Ex) {
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
						panic_msgbox(L"Reset failed!\nCode: 0x%x", ret);
					break;
				case D3D_OK:
					// Shouldn't be possible but should be gracefully handled anyway
					break;
				default:
					// Something definitely went wrong
					panic_msgbox(L"TestCooperativeLevel failed!\nCode: 0x%x", ret);
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
	if (D3D9Overlay::Instance)
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

	// Disable vsync on the parameters
	modify_presentation_parameters(pPresentationParameters);
	
	// Create the device
	IDirect3DDevice9* device = nullptr;
	HRESULT res = 0;

	if (Config::D3D9Ex) {
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
			panic_msgbox(L"CreateDeviceEx failed!\nCode: 0x%x", res);

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
			panic_msgbox(L"CreateDevice failed!\nCode: 0x%x", res);
	}

	*ppReturnedDeviceInterface = device;

	// Set the CPU thread to high priority
	SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);

	// Overwrite some vtable entries in IDirect3DDevice9
	DWORD* device_vtbl = *(DWORD**)device;

	// Take the device's own Present before the game (and the tools it loads) get their hands on
	// the device. This is what the frame interpolation presents its extra frames with (see above)
	present_unhooked = (HRESULT(__stdcall*)(IDirect3DDevice9*, const RECT*, const RECT*, HWND, const RGNDATA*))device_vtbl[17];

	Reset_orig = (HRESULT(__stdcall*)(IDirect3DDevice9*, D3DPRESENT_PARAMETERS*))device_vtbl[16];
	auto patch_data = (DWORD)Reset_hook;
	patch_bytes(&device_vtbl[16], &patch_data, sizeof(DWORD));

	if (Config::D3D9Ex) {
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

// Upgrades IDirect3D9 to IDirect3D9Ex
IDirect3D9* WINAPI Direct3DCreate9_hook(UINT SDKVersion) {
	printf("Direct3DCreate9 intercepted!\n");

	// Window mode: only a fallback for the games that get the window mode question - those
	// are all D3D8 titles, so this normally does nothing
	WindowMode::AskIfNeeded();

	if (Config::D3D9Ex) {
		auto create_ret = Direct3DCreate9Ex(D3D_SDK_VERSION, (IDirect3D9Ex**)&d3d9);
		if (FAILED(create_ret) || d3d9 == nullptr)
			panic_msgbox(L"Direct3DCreate9Ex failed!\nCode: 0x%x", create_ret);
	} else {
		d3d9 = Direct3DCreate9(D3D_SDK_VERSION);
		if (d3d9 == nullptr)
			panic_msgbox(L"Direct3DCreate9 failed!");
	}

	// Hook IDirect3D9::CreateDevice
	DWORD* d3d9_vtbl = *(DWORD**)d3d9;
	CreateDevice_orig = (HRESULT(__stdcall*)(IDirect3D9*, UINT, D3DDEVTYPE, HWND, DWORD, D3DPRESENT_PARAMETERS*, IDirect3DDevice9**))d3d9_vtbl[16];
	auto patch_data = (DWORD)CreateDevice_hook;
	patch_bytes(&d3d9_vtbl[16], &patch_data, sizeof(DWORD));
	return d3d9;
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

// True only if the loaded d3d8.dll really is dgVoodoo2.
// We identify it by its version resource: dgVoodoo2's D3D8.dll reports
// ProductName "dgVoodoo" and FileDescription "dgVoodoo <version> - Direct3D8".
// This is a precise check on purpose - an unknown wrapper should still get the warning.
static bool loaded_d3d8_is_dgvoodoo2() {
	HMODULE mod = GetModuleHandleA("d3d8.dll");
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
			if (loaded_d3d8_is_dgvoodoo2()) {
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
