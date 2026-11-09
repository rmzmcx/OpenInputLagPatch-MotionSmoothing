#pragma once

#include <Windows.h>
#include <dxgi.h>

void x64_overlay_attach(HWND game_window);
void x64_overlay_draw(IDXGISwapChain* swapchain);
void x64_overlay_destroy();
