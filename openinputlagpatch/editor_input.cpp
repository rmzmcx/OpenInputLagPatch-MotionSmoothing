#include <Windows.h>
#define DIRECTINPUT_VERSION 0x0800
#include <dinput.h>
#include <shlwapi.h>
#include <stdio.h>
#include <string.h>
#include "config.h"
#include "editor_input.h"
#include "patch_util.h"

#ifdef OILP_UI_TEXT_EDITOR
namespace {
	using DirectInput8Create_t = HRESULT(WINAPI*)(HINSTANCE, DWORD, REFIID, LPVOID*, LPUNKNOWN);
	using GetDeviceState_t = HRESULT(STDMETHODCALLTYPE*)(IDirectInputDevice8*, DWORD, LPVOID);
	using GetDeviceData_t = HRESULT(STDMETHODCALLTYPE*)(IDirectInputDevice8*, DWORD,
		LPDIDEVICEOBJECTDATA, LPDWORD, DWORD);

	IDirectInput8* dinput;
	IDirectInputDevice8* keyboard;
	GetDeviceState_t real_get_device_state;
	GetDeviceData_t real_get_device_data;
	bool blocked;
	bool installed;
	bool hooked;

	HRESULT STDMETHODCALLTYPE GetDeviceState_hook(IDirectInputDevice8* device, DWORD size, LPVOID state) {
		static bool inside;
		if (inside)
			return DIERR_INPUTLOST;
		inside = true;
		HRESULT result = real_get_device_state != nullptr
			? real_get_device_state(device, size, state) : DIERR_INPUTLOST;
		inside = false;
		if (blocked && size == 256 && state != nullptr)
			memset(state, 0, 256);
		return result;
	}

	HRESULT STDMETHODCALLTYPE GetDeviceData_hook(IDirectInputDevice8* device, DWORD size,
		LPDIDEVICEOBJECTDATA data, LPDWORD count, DWORD flags) {
		static bool inside;
		if (inside)
			return DIERR_INPUTLOST;
		inside = true;
		HRESULT result = real_get_device_data != nullptr
			? real_get_device_data(device, size, data, count, flags) : DIERR_INPUTLOST;
		inside = false;
		if (blocked && count != nullptr) {
			*count = 0;
			return DI_OK;
		}
		return result;
	}

	void HookKeyboard() {
		if (keyboard == nullptr || hooked)
			return;
		hooked = true;
		void** vtable = *(void***)keyboard;
		if (vtable[9] != (void*)GetDeviceState_hook) {
			real_get_device_state = (GetDeviceState_t)vtable[9];
			uintptr_t hook = (uintptr_t)GetDeviceState_hook;
			patch_bytes(&vtable[9], &hook, sizeof(hook));
		}
		if (vtable[10] != (void*)GetDeviceData_hook) {
			real_get_device_data = (GetDeviceData_t)vtable[10];
			uintptr_t hook = (uintptr_t)GetDeviceData_hook;
			patch_bytes(&vtable[10], &hook, sizeof(hook));
		}
	}
}

void EditorInput::Install() {
	if (installed || !Config::DeveloperTextEditor)
		return;
	installed = true;

	wchar_t path[MAX_PATH] = {};
	GetSystemDirectoryW(path, MAX_PATH);
	PathAppendW(path, L"dinput8.dll");
	HMODULE dinput_module = LoadLibraryW(path);
	if (dinput_module == nullptr)
		return;
	auto create = (DirectInput8Create_t)GetProcAddress(dinput_module, "DirectInput8Create");
	if (create == nullptr)
		return;
	if (FAILED(create(GetModuleHandleW(nullptr), DIRECTINPUT_VERSION, IID_IDirectInput8A,
		(void**)&dinput, nullptr)))
		return;
	if (FAILED(dinput->CreateDevice(GUID_SysKeyboard, &keyboard, nullptr)))
		return;
	HookKeyboard();
}

void EditorInput::SetBlocked(bool value) {
	blocked = value;
}
#endif
