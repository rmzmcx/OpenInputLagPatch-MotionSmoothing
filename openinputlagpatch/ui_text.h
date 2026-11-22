#pragma once

#include <stddef.h>
#include <string>
#include <vector>
#include "imgui/imgui.h"

// The overlay's text, separate from the code that draws it. Defaults stay in the source; the
// Developer build edits a UTF-8 override file, while Release compiles baked overrides through
// ui_text_baked.inc.
namespace UiText {
	struct Entry {
		std::string id;
		std::string defaults[3];   // Simplified Chinese, English, Japanese
		std::string overrides[3];
		bool multiline;
	};

	// Defaults are in Simplified Chinese, English and Japanese order.
	void Register(const char* id, const char* zh, const char* en, const char* ja,
		bool multiline = false);

	const char* Get(const char* id);
	const char* Get(const char* id, int language);
	const char* Get(size_t index, int language);
	const char* Default(size_t index, int language);
	bool Find(const char* id, size_t* index);

	void SetOverride(size_t index, int language, const char* text);
	void ResetOverride(size_t index, int language);
	void ResetAllOverrides();
	bool HasOverride(size_t index, int language);

#ifdef OILP_UI_TEXT_EDITOR
	bool Load();
	bool Save();
	const wchar_t* FilePath();
#endif
	void ApplyBaked();
	const char* LanguageLabel(int language);

	void AppendGlyphs(ImVector<ImWchar>& out, int language);

	// Incremented when the current strings have changed enough for the font atlas to need a rebuild.
	void RequestFontRebuild();
	unsigned int FontRevision();

	const std::vector<Entry>& Entries();
}
