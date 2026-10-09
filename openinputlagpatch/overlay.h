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

// Counts one presentation of the current frame. The frame interpolation puts a frame on screen
// more than once per game frame, and this is what tells the overlay's frame rate display how many
// frames per second are actually reaching the display.
void overlay_mark_presentation();

// Whether the overlay shows that frame rate along the bottom of the screen. It is off by default,
// and the interpolation turns it on (together with the overlay itself). The D3D9 overlay is the
// one that draws it: the games that interpolate are D3D9 ones, and a game that never gets a D3D9
// device can't have it on.
void overlay_show_present_rate(bool show);

// Where along the bottom that frame rate is drawn. It sits at the right edge of the window by
// default, which is the bottom right corner of the picture - but that corner is also where the
// user's tools (thprac's frame rate and slowdown readout, for example) draw their own text, and
// the two end up on top of each other. A game that knows where its own picture ends can point the
// line at the right edge of its play area instead, so it sits next to the game's picture rather
// than over the corner of it.
//
// picture_right is where that edge is in the game's own coordinates and picture_width is the width
// of the game's picture in the same coordinates (th15: 416 and 640, the play field being 384 wide
// and starting at x = 32); the overlay scales them to whatever size the back buffer is. Passing 0
// for picture_width goes back to the right edge of the window.
void overlay_set_present_rate_anchor(int picture_right, int picture_width);

// Puts that frame rate right after the line the patch draws in the bottom left corner (the one
// that says how long the game's own render took) instead of at the right edge of the window or of
// the game's picture, so the two readouts sit next to each other. Takes priority over
// overlay_set_present_rate_anchor.
void overlay_set_present_rate_follow_text(bool follow);
