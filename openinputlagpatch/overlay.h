#pragma once

// Bridge between the frame limiter and whichever renderer draws the text overlay.
//
// The overlay is drawn with D3D9 whenever the game (or its D3D8 wrapper) goes through D3D9:
// d3d8to9 and DXVK both do, and so do the native D3D9 games. dgVoodoo2 translates D3D8
// straight to D3D11 instead, so those games never get a D3D9 device and the overlay has to
// be drawn with D3D8 (see d3d8_overlay.cpp).
//
// This header deliberately doesn't include any D3D headers, since the D3D8 and D3D9 headers
// can't be used in the same translation unit.

// True once a D3D9 device exists, i.e. the D3D9 overlay is the one that draws
bool d3d9_overlay_available();

// True while the D3D8 overlay exists and draws
bool d3d8_overlay_active();

// Updates the text the D3D8 overlay draws. Does nothing if it isn't active.
void d3d8_overlay_set_text(const char* text, unsigned long color);
