#pragma once
#include <Windows.h>

void patch_bytes(void* dst, void* src, size_t len);
#ifndef _WIN64
void patch_bytes(DWORD dst, void* src, size_t len);
#endif
void patch_call(void* target, void* func);
void patch_jmp(void* target, void* func);
#ifndef _WIN64
void patch_call(DWORD target, void* func);
void patch_jmp(DWORD target, void* func);
#else
void* install_x64_detour(void* target, void* hook, size_t stolen_bytes);
#endif
void* iat_hook(LPCWSTR target, LPCSTR dll, LPCSTR func, void* hook);
