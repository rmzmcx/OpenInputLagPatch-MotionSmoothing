#pragma once

// In-game ImGui overlay (see imgui_overlay.cpp). The games call into this from their present
// hooks. With Config::UseInGameImguiOverlay off nothing here creates a context, hooks the window
// or loads a font, so the game runs exactly as it did before the overlay existed.
namespace OilpImgui {
	// True when the ini enables the overlay.
	bool Enabled();

	// The panel state, toggled by the overlay hotkey.
	bool IsOpen();
	void Toggle();

	// Builds the frame. Call once per presented frame, after the device is known and before Render.
	void NewFrame();

	// Draws the panels when open, or the startup hint while it lasts.
	void Render();

	// Installs the window hook and the platform backend for the window the game presents to.
	// Returns false when the overlay is disabled or the window is not known yet, so the caller can
	// try again on the next frame.
	bool Install(void* hwnd);

	// Whether the overlay wants the mouse this frame (the panels are open or the hint is showing).
	bool WantsMouse();

	// The hotkey that opens the panels, as configured ("Ctrl+Shift+O" by default). Polled once per
	// frame; the edge is what triggers, so holding the keys does not toggle repeatedly.
	void PollHotkey();
}
