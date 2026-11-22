#pragma once

#ifdef OILP_UI_TEXT_EDITOR
namespace UiTextEditor {
	void SetWindow(void* hwnd);
	bool Enabled();
	bool IsOpen();
	void Toggle();
	void PollHotkey();
	void Draw();
}
#endif
