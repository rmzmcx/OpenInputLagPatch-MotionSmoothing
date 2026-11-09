#pragma once

#include <Windows.h>
#include <d3d11.h>
#include <dxgi1_2.h>

bool d3d11_install_present_hook(uintptr_t game_present_call);
bool d3d11_swapchain_is_flip_discard();
void d3d11_prepare_flip_discard(DXGI_SWAP_CHAIN_DESC1* desc);
void d3d11_prepare_flip_discard(DXGI_SWAP_CHAIN_DESC* desc);
