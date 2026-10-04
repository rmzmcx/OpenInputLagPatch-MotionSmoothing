#pragma once

#include <Windows.h>
#include "d3d8/d3d8.h"

namespace D3D8Hook {
	// Hooks IDirect3DDevice8::Present (vtable index 15) so frames can be skipped when the
	// target framerate is above the display refresh rate (see Limiter::ShouldPresent).
	// Safe to call every frame: it does nothing until a device is available, and only
	// installs the hook once.
	void Install(IDirect3DDevice8* device);
}
