#pragma once

// Lets a tool (thprac) draw its overlay again for the extra presentations of the frame
// interpolation, without running any of the tool's own code (see tool_state.cpp).
//
// The tools draw their overlay in two steps: their UI frame is built from a hook on the return of
// the game's scene update, and it's drawn from a hook on the return of the game's object render.
// There's a value in between that says "the frame is ready to be drawn": set while the frame is
// built, cleared once the tool has drawn it. This finds that value at runtime (the tool is a
// separate module, so there's nothing to look up by name) and sets it again where needed.
namespace ToolState {
	// Call where the tool's frame is ready but not drawn yet - in the game's frame, right before the
	// object render (the tool's draw hook runs on the return of that). frame_address is where the
	// tool builds its frame, draw_address where it draws it.
	void WatchFrameReady(unsigned char* frame_address, unsigned char* draw_address);

	// Call after the tool has drawn its frame (the game's present, for example)
	void WatchFrameDrawn();

	// Readies the tool's frame again, so the next draw_address call (the extra presentation's
	// object render, for example) draws the overlay the tool built. Does nothing until the value
	// has been found, and nothing at all when there's no tool.
	void ReadyToDraw();
}
