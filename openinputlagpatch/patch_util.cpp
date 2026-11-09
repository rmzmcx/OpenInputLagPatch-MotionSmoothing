#include <Windows.h>
#include <stdio.h>
#include <stdint.h>
#include <limits>

#ifdef _WIN64
namespace {
struct AbsoluteJumpStub {
    void* target;
    void* destination;
    void* code;
};

AbsoluteJumpStub absolute_jump_stubs[256];
size_t absolute_jump_stub_count;

void* allocate_near(void* target_address, size_t size) {
    SYSTEM_INFO system_info = {};
    GetSystemInfo(&system_info);
    const uintptr_t granularity = system_info.dwAllocationGranularity;
    const uintptr_t reach = 0x70000000;
    const uintptr_t target = (uintptr_t)target_address;
    uintptr_t lower = target > reach ? target - reach : granularity;
    lower = (lower + granularity - 1) & ~(granularity - 1);
    uintptr_t upper = target + reach;
    if (upper < target || upper > (uintptr_t)-1 - granularity)
        upper = (uintptr_t)-1 - granularity;

    for (uintptr_t address = lower; address < upper; address += granularity) {
        void* code = VirtualAlloc((void*)address, size,
            MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
        if (code != nullptr)
            return code;
    }
    return nullptr;
}

void write_absolute_jump(uint8_t* code, void* destination) {
    BYTE jump[] = { 0x48, 0xB8, 0, 0, 0, 0, 0, 0, 0, 0, 0xFF, 0xE0 };
    memcpy(jump + 2, &destination, sizeof(destination));
    memcpy(code, jump, sizeof(jump));
}

void* closest_absolute_jump_stub(void* patch_target, void* destination) {
    for (size_t i = 0; i < absolute_jump_stub_count; ++i) {
        if (absolute_jump_stubs[i].target == patch_target &&
            absolute_jump_stubs[i].destination == destination) {
            return absolute_jump_stubs[i].code;
        }
    }
    if (absolute_jump_stub_count >= _countof(absolute_jump_stubs))
        return nullptr;

    void* code = allocate_near(patch_target, 12);
    if (code == nullptr)
        return nullptr;
    write_absolute_jump((uint8_t*)code, destination);
    FlushInstructionCache(GetCurrentProcess(), code, 12);
    absolute_jump_stubs[absolute_jump_stub_count++] = { patch_target, destination, code };
    return code;
}
}
#endif

void patch_bytes(void* dst, void* src, size_t len) {
    DWORD prot;
    // Don't write anything if the memory couldn't be made writable (an unknown build of a
    // game, for example): writing anyway would take the whole game down
    if (!VirtualProtect(dst, len, PAGE_READWRITE, &prot)) {
        printf("Failed to make %p writable for patching, skipping!\n", dst);
        return;
    }
    memcpy(dst, src, len);
    FlushInstructionCache(GetCurrentProcess(), dst, len);
    VirtualProtect(dst, len, prot, &prot);
}

void patch_call(void* target, void* func) {
    BYTE patch[5];
    patch[0] = 0xE8;
#ifdef _WIN64
    void* destination = func;
    intptr_t delta = (intptr_t)destination - (intptr_t)target - 5;
    if (delta < (std::numeric_limits<int32_t>::min)() ||
        delta > (std::numeric_limits<int32_t>::max)()) {
        destination = closest_absolute_jump_stub(target, func);
        delta = (intptr_t)destination - (intptr_t)target - 5;
        if (destination == nullptr ||
            delta < (std::numeric_limits<int32_t>::min)() ||
            delta > (std::numeric_limits<int32_t>::max)()) {
            printf("patch_call target %p cannot reach %p with rel32\n", target, func);
            return;
        }
    }
    *(int32_t*)(patch + 1) = (int32_t)delta;
#else
    *(DWORD*)(patch + 1) = (DWORD)func - (DWORD)target - 5;
#endif
    patch_bytes(target, patch, sizeof(patch));
}

void patch_jmp(void* target, void* func) {
    BYTE patch[5];
    patch[0] = 0xE9;
#ifdef _WIN64
    void* destination = func;
    intptr_t delta = (intptr_t)destination - (intptr_t)target - 5;
    if (delta < (std::numeric_limits<int32_t>::min)() ||
        delta > (std::numeric_limits<int32_t>::max)()) {
        destination = closest_absolute_jump_stub(target, func);
        delta = (intptr_t)destination - (intptr_t)target - 5;
        if (destination == nullptr ||
            delta < (std::numeric_limits<int32_t>::min)() ||
            delta > (std::numeric_limits<int32_t>::max)()) {
            printf("patch_jmp target %p cannot reach %p with rel32\n", target, func);
            return;
        }
    }
    *(int32_t*)(patch + 1) = (int32_t)delta;
#else
    *(DWORD*)(patch + 1) = (DWORD)func - (DWORD)target - 5;
#endif
    patch_bytes(target, patch, sizeof(patch));
}

#ifdef _WIN64
void* install_x64_detour(void* target, void* hook, size_t stolen_bytes) {
    if (target == nullptr || hook == nullptr || stolen_bytes < 5)
        return nullptr;
    uint8_t* trampoline = (uint8_t*)allocate_near(target, stolen_bytes + 12);
    if (trampoline == nullptr)
        return nullptr;
    memcpy(trampoline, target, stolen_bytes);
    write_absolute_jump(trampoline + stolen_bytes,
        (uint8_t*)target + stolen_bytes);
    FlushInstructionCache(GetCurrentProcess(), trampoline, stolen_bytes + 12);
    patch_jmp(target, hook);
    return trampoline;
}
#endif

#ifndef _WIN64
void patch_bytes(DWORD dst, void* src, size_t len) {
    patch_bytes((void*)dst, src, len);
}

void patch_call(DWORD target, void* func) {
    patch_call((void*)target, func);
}

void patch_jmp(DWORD target, void* func) {
    patch_jmp((void*)target, func);
}
#endif

void* iat_hook(LPCWSTR target, LPCSTR dll, LPCSTR func, void* hook) {
    // Get a pointer to the module
    auto pe = (char*)GetModuleHandleW(target);
    if (!pe)
        return nullptr;

    // Get the PE optional header
#ifdef _WIN64
    auto nt_header = (PIMAGE_NT_HEADERS64)(pe + ((PIMAGE_DOS_HEADER)pe)->e_lfanew);
    auto optional_header = (PIMAGE_OPTIONAL_HEADER64)(&nt_header->OptionalHeader);
#else
    auto nt_header = (PIMAGE_NT_HEADERS32)(pe + ((PIMAGE_DOS_HEADER)pe)->e_lfanew);
    auto optional_header = (PIMAGE_OPTIONAL_HEADER32)(&nt_header->OptionalHeader);
#endif

    // Find the dll's import descriptor
    bool found = false;
    auto import_desc = (PIMAGE_IMPORT_DESCRIPTOR)(pe + optional_header->DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT].VirtualAddress);
    for (; import_desc->Name; import_desc++) {
        if (!_stricmp(pe + import_desc->Name, dll)) {
            found = true;
            break;
        }
    }
    if (!found)
        return nullptr;

    // Get the target function's pointer
    auto dll_handle = GetModuleHandleA(dll);
    if (!dll_handle)
        return nullptr;
    auto target_ptr = (uintptr_t)GetProcAddress(dll_handle, func);
    if (!target_ptr)
        return nullptr;

    // Find and replace the target function with the hook
#ifdef _WIN64
    auto thunk = (PIMAGE_THUNK_DATA64)(pe + import_desc->FirstThunk);
#else
    auto thunk = (PIMAGE_THUNK_DATA32)(pe + import_desc->FirstThunk);
#endif
    for (; thunk->u1.Function != NULL; thunk++) {
        if (thunk->u1.Function == target_ptr) {
            printf("Overriding function %s from %s in %S\n", func, dll, target ? target : L"main executable");
            patch_bytes(&thunk->u1.Function, &hook, sizeof(hook));
        }
    }

    return (void*)target_ptr;
}
