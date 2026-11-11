#include <stdio.h>
#include "d3d11_hook.h"
#include "overlay.h"
#include "patch_util.h"
#include "x64_overlay.h"

namespace {
IDXGISwapChain* hooked_swapchain;
bool present_hook_installed;
bool logged_swapchain;
bool swapchain_is_flip_discard;
bool swapchain_allows_tearing;
IDXGISwapChain* capability_swapchain;

void update_swapchain_capabilities(IDXGISwapChain* swapchain) {
	if (swapchain == capability_swapchain)
		return;
	capability_swapchain = swapchain;
	swapchain_allows_tearing = false;

	IDXGISwapChain1* swapchain1 = nullptr;
	if (FAILED(swapchain->QueryInterface(
			__uuidof(IDXGISwapChain1), (void**)&swapchain1)) ||
		swapchain1 == nullptr) {
		return;
	}
	DXGI_SWAP_CHAIN_DESC1 desc1 = {};
	if (SUCCEEDED(swapchain1->GetDesc1(&desc1))) {
		swapchain_allows_tearing =
			(desc1.Flags & DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING) != 0;
	}
	swapchain1->Release();
}

HRESULT present_swapchain(IDXGISwapChain* swapchain, UINT flags) {
	update_swapchain_capabilities(swapchain);
	DXGI_SWAP_CHAIN_DESC desc = {};
	if (SUCCEEDED(swapchain->GetDesc(&desc))) {
		if (desc.OutputWindow != nullptr)
			x64_overlay_attach(desc.OutputWindow);
		swapchain_is_flip_discard =
			desc.SwapEffect == DXGI_SWAP_EFFECT_FLIP_DISCARD;
		if (!logged_swapchain) {
			logged_swapchain = true;
			printf("D3D11 swap chain: effect %u, buffers %u, %ux%u\n",
				(unsigned int)desc.SwapEffect, desc.BufferCount,
				desc.BufferDesc.Width, desc.BufferDesc.Height);
		}
	}

	const bool test_present = (flags & DXGI_PRESENT_TEST) != 0;
	if (!test_present)
		x64_overlay_draw(swapchain);
	UINT present_flags = flags;
	if (swapchain_allows_tearing && !test_present)
		present_flags |= DXGI_PRESENT_ALLOW_TEARING;
	const HRESULT result = swapchain->Present(0, present_flags);
	if (SUCCEEDED(result) && !test_present)
		overlay_mark_presentation();
	return result;
}

HRESULT STDMETHODCALLTYPE present_hook(
	IDXGISwapChain* swapchain, UINT, UINT flags) {
	if (swapchain == nullptr)
		return E_POINTER;
	hooked_swapchain = swapchain;
	return present_swapchain(swapchain, flags);
}
}

bool d3d11_install_present_hook(uintptr_t game_present_call) {
	if (present_hook_installed)
		return true;
	if (game_present_call == 0)
		return false;
	patch_call((void*)game_present_call, (void*)&present_hook);
	present_hook_installed = true;
	printf("D3D11: IDXGISwapChain::Present call hook installed at %p\n",
		(void*)game_present_call);
	return true;
}

bool d3d11_swapchain_is_flip_discard() {
	return swapchain_is_flip_discard;
}

bool d3d11_swapchain_allows_tearing() {
	return swapchain_allows_tearing;
}

void d3d11_prepare_flip_discard(DXGI_SWAP_CHAIN_DESC1* desc,
	bool allow_tearing) {
	if (desc == nullptr)
		return;
	desc->SampleDesc.Count = 1;
	desc->SampleDesc.Quality = 0;
	desc->BufferCount = desc->BufferCount < 2 ? 2 : desc->BufferCount;
	desc->Scaling = DXGI_SCALING_STRETCH;
	desc->SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
	desc->AlphaMode = DXGI_ALPHA_MODE_IGNORE;
	desc->Flags &= ~(DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH |
		DXGI_SWAP_CHAIN_FLAG_GDI_COMPATIBLE);
	if (allow_tearing)
		desc->Flags |= DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING;
	else
		desc->Flags &= ~DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING;
}

void d3d11_prepare_flip_discard(DXGI_SWAP_CHAIN_DESC* desc,
	bool allow_tearing) {
	if (desc == nullptr)
		return;
	desc->SampleDesc.Count = 1;
	desc->SampleDesc.Quality = 0;
	desc->BufferCount = desc->BufferCount < 2 ? 2 : desc->BufferCount;
	desc->SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
	desc->Flags &= ~(DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH |
		DXGI_SWAP_CHAIN_FLAG_GDI_COMPATIBLE);
	if (allow_tearing)
		desc->Flags |= DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING;
	else
		desc->Flags &= ~DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING;
}
