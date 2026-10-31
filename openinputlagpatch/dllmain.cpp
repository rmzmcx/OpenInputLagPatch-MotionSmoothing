#include <Windows.h>
#include <stdio.h>
#include <winternl.h>
#include <shlwapi.h>
#include <intrin.h>
#include "patch_util.h"
#include "games.h"
#include "limiter.h"
#include "d3d9_hook.h"
#include "d3d8_hook.h"
#include "d3dx9_hook.h"
#include "config.h"
#include "sha256.h"
#include "common.h"
#include "window_mode.h"

// Allocates a console for debugging purposes
void create_console() {
    AllocConsole();
    freopen("CONOUT$", "w", stdout);
}

// Register a DLL load callback to abort if vpatch gets loaded
typedef struct _LDR_DLL_LOADED_NOTIFICATION_DATA
{
    ULONG Flags;                   // Reserved.
    PCUNICODE_STRING FullDllName;  // The full path name of the DLL module.
    PCUNICODE_STRING BaseDllName;  // The base file name of the DLL module.
    PVOID DllBase;                 // A pointer to the base address for the DLL in memory.
    ULONG SizeOfImage;             // The size of the DLL image, in bytes.
} LDR_DLL_LOADED_NOTIFICATION_DATA, *PLDR_DLL_LOADED_NOTIFICATION_DATA;
#define LDR_DLL_NOTIFICATION_REASON_LOADED 1

// MessageBox is broken in dll_load_callback, so vpatch's entrypoint is redirected to this instead
void vpatch_abort() {
    panic_msgbox(UiMessage_VpatchIncompatible);
}

VOID CALLBACK dll_load_callback(ULONG NotificationReason, PLDR_DLL_LOADED_NOTIFICATION_DATA NotificationData, PVOID Context) {
    // Only notify on DLL load
    if (NotificationReason != LDR_DLL_NOTIFICATION_REASON_LOADED)
        return;

    // Check if the DLL's filename contains vpatch
    if (wcsstr(NotificationData->BaseDllName->Buffer, L"vpatch") != nullptr) {
        printf("vpatch detected: %S\n", NotificationData->FullDllName->Buffer);

        // Patch vpatch's entrypoint to abort instantly
        auto pe = (char*)NotificationData->DllBase;
        auto nt_header = (PIMAGE_NT_HEADERS32)(pe + ((PIMAGE_DOS_HEADER)pe)->e_lfanew);
        auto optional_header = (PIMAGE_OPTIONAL_HEADER32)(&nt_header->OptionalHeader);
        patch_call(pe + optional_header->AddressOfEntryPoint, vpatch_abort);
    }
}

// Typedef edited to remove LDR_DLL_UNLOADED_NOTIFICATION_DATA
typedef VOID NTAPI LDR_DLL_NOTIFICATION_FUNCTION(_In_ ULONG NotificationReason,
    _In_ PLDR_DLL_LOADED_NOTIFICATION_DATA NotificationData,
    _In_opt_ PVOID Context);
typedef LDR_DLL_NOTIFICATION_FUNCTION* PLDR_DLL_NOTIFICATION_FUNCTION;
typedef NTSTATUS(NTAPI* LdrRegisterDllNotification_t)(
    _In_ ULONG Flags, _In_ PLDR_DLL_NOTIFICATION_FUNCTION NotificationFunction,
    _In_opt_ PVOID Context, _Out_ PVOID* Cookie);
void register_dll_load_callback() {
    // Get LdrRegisterDllNotification from ntdll.dll
    auto ntdll = GetModuleHandleW(L"ntdll.dll");
    if (!ntdll)
        return;
    auto LdrRegisterDllNotification = (LdrRegisterDllNotification_t)GetProcAddress(ntdll, "LdrRegisterDllNotification");
    if (!LdrRegisterDllNotification)
        return;

    // Register the DLL load callback
    void* cookie = nullptr;
    LdrRegisterDllNotification(0, dll_load_callback, NULL, &cookie);
}

// Check if vpatch is loaded and register a DLL load callback to detect it being loaded later
void check_vpatch() {
    // On older Touhou games, vpatch remotely creates a thread that calls LoadLibraryA on itself
    // Since the DLL isn't actually loaded by the time this executes, a callback is registered to detect it being loaded later
    register_dll_load_callback();

    // On modern Touhou games, vpatch itself loads dinput8.dll
    // Get the executable's path
    wchar_t exe_path[1024] = {};
    if (!GetModuleFileName(NULL, exe_path, 1024))
        return;

    // Strip away the path to get the filename itself
    wchar_t* filename = PathFindFileNameW(exe_path);

    // Check if it contains vpatch
    if (wcsstr(filename, L"vpatch") != nullptr) {
        printf("vpatch detected: %S\n", exe_path);
        vpatch_abort();
    }
}

// IAT hook timeBeginPeriod and timeEndPeriod to stub them
// The game calls these functions a bunch of times to set the system timer to 1ms
// It's not very effective because it only sets the timer for incredibly small periods of time
// This ensures that the system timer is set to 1ms for the entirety of the process' lifetime
MMRESULT WINAPI time_stub(UINT uPeriod) {
    return MMSYSERR_NOERROR;
}

void hook_winmm_time_period() {
    // Set the system timer to 1ms
    timeBeginPeriod(1);

    // Replace timeBeginPeriod and timeEndPeriod with a stub
    iat_hook(NULL, "winmm.dll", "timeBeginPeriod", time_stub);
    iat_hook(NULL, "winmm.dll", "timeEndPeriod", time_stub);
}

// TH14 and above break when they're forced to use a refresh rate above 60hz in fullscreen with these patches
// This bypasses that by spoofing a 60hz display mode to the game
int __stdcall GetDeviceCaps_hook(HDC hdc, int index) {
    if (index == VREFRESH)
        return 60;
    return GetDeviceCaps(hdc, index);
}

void hook_gdi32() {
    iat_hook(NULL, "gdi32.dll", "GetDeviceCaps", GetDeviceCaps_hook);
}

// Steam releases are wrapped with SteamStub, which keeps .text encrypted until its own entrypoint has run
// Patching before that would get scrambled by the decryption, so patches are delayed until the game's code is running
static TouhouGame steamstub_game = TouhouGame::Unknown;
static void(WINAPI* GetSystemTimeAsFileTime_orig)(LPFILETIME) = nullptr;
static void(WINAPI* GetStartupInfoW_orig)(LPSTARTUPINFOW) = nullptr;
static LPSTR(WINAPI* GetCommandLineA_orig)() = nullptr;
static LPWSTR(WINAPI* GetCommandLineW_orig)() = nullptr;
static HMODULE(WINAPI* GetModuleHandleW_orig)(LPCWSTR) = nullptr;
static BOOL(WINAPI* QueryPerformanceCounter_orig)(LARGE_INTEGER*) = nullptr;
static char* text_start = nullptr;
static char* text_end = nullptr;

void install_game_patches(TouhouGame game);

bool find_steamstub() {
    auto pe = (char*)GetModuleHandleW(NULL);
    auto nt_header = (PIMAGE_NT_HEADERS32)(pe + ((PIMAGE_DOS_HEADER)pe)->e_lfanew);
    auto section = IMAGE_FIRST_SECTION(nt_header);
    bool found = false;
    for (WORD i = 0; i < nt_header->FileHeader.NumberOfSections; i++, section++) {
        if (!strncmp((char*)section->Name, ".text", IMAGE_SIZEOF_SHORT_NAME)) {
            text_start = pe + section->VirtualAddress;
            text_end = text_start + section->Misc.VirtualSize;
        }
        else if (!strncmp((char*)section->Name, ".bind", IMAGE_SIZEOF_SHORT_NAME)) {
            found = true;
        }
    }
    return found && text_start;
}

void WINAPI GetSystemTimeAsFileTime_hook(LPFILETIME);
void WINAPI GetStartupInfoW_hook(LPSTARTUPINFOW);
LPSTR WINAPI GetCommandLineA_hook();
LPWSTR WINAPI GetCommandLineW_hook();
HMODULE WINAPI GetModuleHandleW_hook(LPCWSTR);
BOOL WINAPI QueryPerformanceCounter_hook(LARGE_INTEGER*);

// The game's CRT startup code calls some of these through the IAT, right after SteamStub is done
void steamstub_hook_triggers() {
    GetSystemTimeAsFileTime_orig = (decltype(GetSystemTimeAsFileTime_orig))iat_hook(NULL, "kernel32.dll", "GetSystemTimeAsFileTime", GetSystemTimeAsFileTime_hook);
    GetStartupInfoW_orig = (decltype(GetStartupInfoW_orig))iat_hook(NULL, "kernel32.dll", "GetStartupInfoW", GetStartupInfoW_hook);
    GetCommandLineA_orig = (decltype(GetCommandLineA_orig))iat_hook(NULL, "kernel32.dll", "GetCommandLineA", GetCommandLineA_hook);
    GetCommandLineW_orig = (decltype(GetCommandLineW_orig))iat_hook(NULL, "kernel32.dll", "GetCommandLineW", GetCommandLineW_hook);
    GetModuleHandleW_orig = (decltype(GetModuleHandleW_orig))iat_hook(NULL, "kernel32.dll", "GetModuleHandleW", GetModuleHandleW_hook);
    QueryPerformanceCounter_orig = (decltype(QueryPerformanceCounter_orig))iat_hook(NULL, "kernel32.dll", "QueryPerformanceCounter", QueryPerformanceCounter_hook);
}

// The first of these functions called from the game's own code means SteamStub is done
// The hooks stay in place afterwards since iat_hook can't restore them, but they just pass through
void steamstub_check(void* caller) {
    if (steamstub_game == TouhouGame::Unknown || (char*)caller < text_start || (char*)caller >= text_end)
        return;
    auto game = steamstub_game;
    steamstub_game = TouhouGame::Unknown;
    printf("SteamStub finished, installing patches\n");
    install_game_patches(game);
}

void WINAPI GetSystemTimeAsFileTime_hook(LPFILETIME time) {
    steamstub_check(_ReturnAddress());
    GetSystemTimeAsFileTime_orig(time);
}

void WINAPI GetStartupInfoW_hook(LPSTARTUPINFOW info) {
    steamstub_check(_ReturnAddress());
    GetStartupInfoW_orig(info);
}

LPSTR WINAPI GetCommandLineA_hook() {
    steamstub_check(_ReturnAddress());
    return GetCommandLineA_orig();
}

LPWSTR WINAPI GetCommandLineW_hook() {
    steamstub_check(_ReturnAddress());
    return GetCommandLineW_orig();
}

HMODULE WINAPI GetModuleHandleW_hook(LPCWSTR name) {
    steamstub_check(_ReturnAddress());
    return GetModuleHandleW_orig(name);
}

BOOL WINAPI QueryPerformanceCounter_hook(LARGE_INTEGER* count) {
    steamstub_check(_ReturnAddress());
    return QueryPerformanceCounter_orig(count);
}

// Actually patches everything
void install_patches() {
    auto game = detect_game();
    if (game == TouhouGame::Unknown)
        return;
    else if (game == TouhouGame::Custom)
        return;

    if (find_steamstub()) {
        printf("SteamStub detected, delaying patches\n");
        steamstub_game = game;
        steamstub_hook_triggers();
        return;
    }
    install_game_patches(game);
}

void install_game_patches(TouhouGame game) {
    Limiter::Initialize(get_replay_callback(game));
    hook_winmm_time_period();
    hook_gdi32();
    D3D8Hook::HookDirect3DCreate8();
    hook_d3d9();
    if (Config::D3D9Ex)
        hook_d3dx9();

    auto patch_function = get_patch_function(game);
    if (!patch_function)
        panic_msgbox(UiMessage_MissingPatchFunction);
    patch_function();
}

// Main entrypoint
void patcher_main() {
    __int64 start;
    __int64 end;
    __int64 freq;
    QueryPerformanceFrequency((LARGE_INTEGER*)&freq);
    QueryPerformanceCounter((LARGE_INTEGER*)&start);

    Config::Load();
    if (Config::DebugWait)
        queue_ui_message(UiMessage_DebugWait, 0, false);
    if (Config::DebugConsole)
        create_console();
    WindowMode::Hook();
    check_vpatch();
    install_patches();

    QueryPerformanceCounter((LARGE_INTEGER*)&end);
    printf("Initialized in %f seconds\n", (double)(end - start) / (double)freq);
}

BOOL APIENTRY DllMain( HMODULE hModule,
                       DWORD  ul_reason_for_call,
                       LPVOID lpReserved
                     )
{
    switch (ul_reason_for_call)
    {
    case DLL_PROCESS_ATTACH: {
        patcher_main();
        break;
    }
    case DLL_THREAD_ATTACH:
    case DLL_THREAD_DETACH:
    case DLL_PROCESS_DETACH:
        break;
    }
    return TRUE;
}

HRESULT WINAPI DirectInput8Create(HINSTANCE hinst, DWORD dwVersion,
    REFIID riidltf, LPVOID* ppvOut, LPUNKNOWN punkOuter)
{
    wchar_t real_dinput8_path[MAX_PATH] = {};
    GetSystemDirectoryW(real_dinput8_path, sizeof(real_dinput8_path) / sizeof(wchar_t));
    real_dinput8_path[MAX_PATH - 1] = 0;
    wcscat_s(real_dinput8_path, L"\\dinput8.dll");

    auto real_dinput8 = LoadLibraryW(real_dinput8_path);
    if (real_dinput8) {
        auto real_create = (HRESULT(WINAPI*)(HINSTANCE, DWORD, REFIID, LPVOID*, LPUNKNOWN))GetProcAddress(real_dinput8, "DirectInput8Create");
        if (real_create)
            return real_create(hinst, dwVersion, riidltf, ppvOut, punkOuter);
    }

    return E_FAIL;
}
