#include <Windows.h>
#include <stdio.h>
#include <stdint.h>
#include <winternl.h>
#include <shlwapi.h>
#include <intrin.h>
#include <tlhelp32.h>
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
    setvbuf(stdout, nullptr, _IONBF, 0);
}

static volatile LONG crash_log_written;

static bool is_fatal_exception(DWORD code) {
    switch (code) {
    case EXCEPTION_ACCESS_VIOLATION:
    case EXCEPTION_ILLEGAL_INSTRUCTION:
    case EXCEPTION_PRIV_INSTRUCTION:
    case EXCEPTION_INT_DIVIDE_BY_ZERO:
    case EXCEPTION_IN_PAGE_ERROR:
    case EXCEPTION_STACK_OVERFLOW:
        return true;
    default:
        return false;
    }
}

static void log_memory_region(FILE* file, const char* label, const void* address) {
    MEMORY_BASIC_INFORMATION info = {};
    if (VirtualQuery(address, &info, sizeof(info)) == 0) {
        fprintf(file, "%s region %p: VirtualQuery failed, error %lu\n",
                label, address, GetLastError());
        return;
    }
    fprintf(file,
            "%s region %p: base=%p allocation=%p size=0x%lx state=0x%lx protect=0x%lx type=0x%lx\n",
            label, address, info.BaseAddress, info.AllocationBase, info.RegionSize,
            info.State, info.Protect, info.Type);
}

static void log_code_bytes(FILE* file, const void* address) {
    MEMORY_BASIC_INFORMATION info = {};
    if (VirtualQuery(address, &info, sizeof(info)) == 0 ||
        info.State != MEM_COMMIT ||
        (info.Protect & (PAGE_NOACCESS | PAGE_EXECUTE)) == PAGE_NOACCESS) {
        return;
    }

    const size_t before = 16;
    const size_t after = 48;
    const uintptr_t start = (uintptr_t)address - before;
    BYTE bytes[before + after] = {};
    SIZE_T read = 0;
    if (!ReadProcessMemory(GetCurrentProcess(), (LPCVOID)start, bytes, sizeof(bytes), &read))
        return;

    fprintf(file, "code bytes around %p (start %p, read %lu):\n", address,
            (void*)start, (unsigned long)read);
    for (SIZE_T row = 0; row < read; row += 16) {
        fprintf(file, "  %p:", (void*)(start + row));
        const SIZE_T end = min(read, row + 16);
        for (SIZE_T i = row; i < end; ++i)
            fprintf(file, " %02x", bytes[i]);
        fprintf(file, "\n");
    }
}

// Records the first fatal exception before Windows Error Reporting tears the process down. This is
// intentionally a vectored handler: the dgVoodoo2 failure happens outside the game's own code, and
// the normal WER event does not name the containing module.
static LONG WINAPI oilp_crash_handler(EXCEPTION_POINTERS* exception_info) {
    if (exception_info == nullptr || exception_info->ExceptionRecord == nullptr ||
        !is_fatal_exception(exception_info->ExceptionRecord->ExceptionCode) ||
        InterlockedCompareExchange(&crash_log_written, 1, 0) != 0) {
        return EXCEPTION_CONTINUE_SEARCH;
    }

    wchar_t path[MAX_PATH] = {};
    if (GetModuleFileNameW(nullptr, path, _countof(path)) != 0) {
        PathRemoveFileSpecW(path);
        PathAppendW(path, L"oilp_crash.txt");
    }

    FILE* file = nullptr;
    if (path[0] == L'\0' || _wfopen_s(&file, path, L"w") != 0 || file == nullptr)
        return EXCEPTION_CONTINUE_SEARCH;

    const EXCEPTION_RECORD* record = exception_info->ExceptionRecord;
    fprintf(file, "OpenInputLagPatch fatal exception\n");
    fprintf(file, "code: 0x%08lx\n", record->ExceptionCode);
    fprintf(file, "address: 0x%p\n", record->ExceptionAddress);
    fprintf(file, "flags: 0x%08lx\n", record->ExceptionFlags);
    fprintf(file, "parameters: %lu\n", record->NumberParameters);
    for (DWORD i = 0; i < record->NumberParameters && i < EXCEPTION_MAXIMUM_PARAMETERS; ++i)
        fprintf(file, "  [%lu] 0x%p\n", i, (void*)record->ExceptionInformation[i]);
    if (record->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && record->NumberParameters >= 2) {
        const char* operation = "unknown";
        if (record->ExceptionInformation[0] == 0)
            operation = "read";
        else if (record->ExceptionInformation[0] == 1)
            operation = "write";
        else if (record->ExceptionInformation[0] == 8)
            operation = "execute";
        fprintf(file, "access violation: %s at 0x%p\n", operation,
                (void*)record->ExceptionInformation[1]);
    }

    const CONTEXT* context = exception_info->ContextRecord;
    if (context != nullptr) {
#ifdef _M_IX86
        fprintf(file,
                "registers: eip=0x%08lx esp=0x%08lx ebp=0x%08lx eax=0x%08lx "
                "ebx=0x%08lx ecx=0x%08lx edx=0x%08lx esi=0x%08lx edi=0x%08lx\n",
                context->Eip, context->Esp, context->Ebp, context->Eax, context->Ebx,
                context->Ecx, context->Edx, context->Esi, context->Edi);
#endif
    }

    log_memory_region(file, "exception", record->ExceptionAddress);
    if (record->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && record->NumberParameters >= 2)
        log_memory_region(file, "target", (const void*)record->ExceptionInformation[1]);
    log_code_bytes(file, record->ExceptionAddress);

    fprintf(file, "modules:\n");
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32,
                                               GetCurrentProcessId());
    if (snapshot != INVALID_HANDLE_VALUE) {
        MODULEENTRY32W module = {};
        module.dwSize = sizeof(module);
        if (Module32FirstW(snapshot, &module)) {
            do {
                fprintf(file, "  %p-%p %ls\n", module.modBaseAddr,
                        module.modBaseAddr + module.modBaseSize, module.szModule);
            } while (Module32NextW(snapshot, &module));
        }
        CloseHandle(snapshot);
    } else {
        fprintf(file, "  module snapshot failed: %lu\n", GetLastError());
    }

    fclose(file);
    printf("OpenInputLagPatch wrote oilp_crash.txt\n");
    return EXCEPTION_CONTINUE_SEARCH;
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
    AddVectoredExceptionHandler(1, oilp_crash_handler);

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
    printf("Force 4:3 presentation: %s\n",
        Config::ForceRatio43 ? "enabled" : "disabled");
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
