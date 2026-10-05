#pragma once

#include <Windows.h>
#include "d3d8/d3d8.h"

#ifndef OVERLAY_MAX_CHARS
#define OVERLAY_MAX_CHARS 128
#endif

// Same overlay as D3D9Overlay, but drawn with D3D8. It is needed for D3D8 games whose
// wrapper never goes through D3D9 (dgVoodoo2 translates D3D8 straight to D3D11), because
// those games never get a D3D9 device that the D3D9 overlay could draw with.
class D3D8Overlay {
public:
	// TODO: Release this properly instead of leaking it...
	static D3D8Overlay* Instance;

	D3D8Overlay(IDirect3DDevice8* device, int width, int height);
	~D3D8Overlay();

	void Draw();
	void SetText(const char* format, ...);

	D3DCOLOR text_color;

private:
	void SetupRenderState();
	void SetupResources();
	void SetupAtlasUVTable();

	int UpdateBuffers(char* text);

	IDirect3DDevice8* d3d8_device;
	// D3D8 state blocks are tokens instead of objects like in D3D9
	DWORD d3d8_state_block;
	IDirect3DTexture8* d3d8_font_tex;
	IDirect3DVertexBuffer8* d3d8_vertex_buf;
	IDirect3DIndexBuffer8* d3d8_index_buf;
	int window_width;
	int window_height;
	float atlas_uvs[128 - 32][2];
	float char_width;
	float char_height;
	char text_buffer[OVERLAY_MAX_CHARS];
};
