#include <Windows.h>
#include <imm.h>
#include <stdio.h>
#include <string.h>
#include <string>
#include <vector>
#include "config.h"
#include "editor_input.h"
#include "imgui/imgui.h"
#include "ui_text.h"
#include "ui_text_editor.h"

#ifdef OILP_UI_TEXT_EDITOR
namespace {
	bool open;
	bool hotkey_down;
	HWND window;
	HIMC owned_ime_context;
	char filter[128];
	std::string status;
	bool refresh_buffers;
	std::vector<std::string> buffers[3];

	int Language() {
		if (Config::Language < 0 || Config::Language > 2)
			return 0;
		return Config::Language;
	}

	int ResizeCallback(ImGuiInputTextCallbackData* data) {
		if (data->EventFlag == ImGuiInputTextFlags_CallbackResize) {
			std::string* text = (std::string*)data->UserData;
			text->resize(data->BufTextLen);
			data->Buf = (char*)text->data();
		}
		return 0;
	}

	void RefreshBuffers() {
		const std::vector<UiText::Entry>& entries = UiText::Entries();
		for (int language = 0; language < 3; ++language) {
			buffers[language].resize(entries.size());
			for (size_t i = 0; i < entries.size(); ++i) {
				buffers[language][i] = UiText::Get(i, language);
				buffers[language][i].reserve(256);
			}
		}
	}

	void EnsureBuffers() {
		if (buffers[0].size() == UiText::Entries().size())
			return;
		RefreshBuffers();
	}

	void SetImeEnabled(bool enabled) {
		if (window == nullptr)
			return;
		if (enabled) {
			HIMC current = ImmGetContext(window);
			if (current != nullptr) {
				ImmReleaseContext(window, current);
				return;
			}
			if (owned_ime_context == nullptr)
				owned_ime_context = ImmCreateContext();
			if (owned_ime_context != nullptr)
				ImmAssociateContext(window, owned_ime_context);
		} else if (owned_ime_context != nullptr) {
			ImmAssociateContext(window, nullptr);
			ImmDestroyContext(owned_ime_context);
			owned_ime_context = nullptr;
		}
	}
}

namespace UiTextEditor {
	void SetWindow(void* hwnd) {
		HWND next = (HWND)hwnd;
		if (next == window)
			return;
		SetImeEnabled(false);
		window = next;
		if (open)
			SetImeEnabled(true);
	}

	bool Enabled() {
		return Config::DeveloperTextEditor != FALSE;
	}

	bool IsOpen() {
		return open && Enabled();
	}

	void Toggle() {
		if (!Enabled())
			return;
		open = !open;
		EditorInput::SetBlocked(open);
		if (open) {
			RefreshBuffers();
			SetImeEnabled(true);
		} else {
			SetImeEnabled(false);
		}
	}

	void PollHotkey() {
		if (!Enabled()) {
			open = false;
			EditorInput::SetBlocked(false);
			SetImeEnabled(false);
			return;
		}
		const bool down = (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0
			&& (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0
			&& (GetAsyncKeyState('E') & 0x8000) != 0;
		if (down && !hotkey_down)
			Toggle();
		hotkey_down = down;
	}

	void Draw() {
		if (!open || !Enabled())
			return;
		EditorInput::SetBlocked(true);
		EnsureBuffers();

		ImGui::SetNextWindowSize(ImVec2(820.0f, 620.0f), ImGuiCond_FirstUseEver);
		if (!ImGui::Begin("UI text editor (Developer)", &open)) {
			EditorInput::SetBlocked(false);
			SetImeEnabled(false);
			ImGui::End();
			return;
		}

		if (ImGui::Button("Save")) {
			status = UiText::Save() ? "Saved to ui_text.ini" : "Could not save ui_text.ini";
			UiText::RequestFontRebuild();
		}
		ImGui::SameLine();
		if (ImGui::Button("Reload")) {
			status = UiText::Load() ? "Reloaded ui_text.ini" : "No ui_text.ini to reload";
			refresh_buffers = true;
		}
		ImGui::SameLine();
		if (ImGui::Button("Reset all")) {
			UiText::ResetAllOverrides();
			UiText::RequestFontRebuild();
			status = "Using built-in text";
			refresh_buffers = true;
		}
		ImGui::SameLine();
		if (ImGui::Button("Rebuild font"))
			UiText::RequestFontRebuild();

		if (refresh_buffers) {
			RefreshBuffers();
			refresh_buffers = false;
		}

		if (!status.empty())
			ImGui::TextDisabled("%s", status.c_str());
		ImGui::TextDisabled("Empty text uses the built-in value. Save writes %%APPDATA%%\\"
			"ShanghaiAlice\\oilp_custom\\ui_text.ini. New glyphs appear after clicking away, "
			"or with Rebuild font.");

		const int language = Language();
		ImGui::Text("Editing: %s", UiText::LanguageLabel(language));
		ImGui::SetNextItemWidth(-FLT_MIN);
		ImGui::InputTextWithHint("##filter", "Filter by ID or text", filter, sizeof(filter));

		ImGui::BeginChild("##entries", ImVec2(0.0f, 0.0f), true);
		const std::vector<UiText::Entry>& entries = UiText::Entries();
		for (size_t i = 0; i < entries.size(); ++i) {
			const UiText::Entry& entry = entries[i];
			std::string& buffer = buffers[language][i];
			if (filter[0] != '\0' &&
				strstr(entry.id.c_str(), filter) == nullptr &&
				strstr(buffer.c_str(), filter) == nullptr)
				continue;

			ImGui::PushID((int)i);
			ImGui::TextDisabled("%s", entry.id.c_str());
			const ImGuiInputTextFlags flags =
				ImGuiInputTextFlags_CallbackResize | ImGuiInputTextFlags_AllowTabInput;
			bool changed;
			if (entry.multiline) {
				changed = ImGui::InputTextMultiline("##text", (char*)buffer.c_str(),
					buffer.capacity() + 1, ImVec2(-FLT_MIN, 68.0f), flags,
					ResizeCallback, &buffer);
			} else {
				ImGui::SetNextItemWidth(-FLT_MIN);
				changed = ImGui::InputText("##text", (char*)buffer.c_str(),
					buffer.capacity() + 1, flags, ResizeCallback, &buffer);
			}
			if (changed)
				UiText::SetOverride(i, language, buffer.c_str());
			if (ImGui::IsItemDeactivatedAfterEdit()) {
				if (buffer.empty()) {
					buffer = UiText::Get(i, language);
					buffer.reserve(256);
				}
				UiText::RequestFontRebuild();
			}

			if (UiText::HasOverride(i, language)) {
				if (ImGui::SmallButton("Reset")) {
					UiText::ResetOverride(i, language);
					buffer = UiText::Default(i, language);
					buffer.reserve(256);
					UiText::RequestFontRebuild();
				}
			}
			ImGui::Separator();
			ImGui::PopID();
		}
		ImGui::EndChild();
		ImGui::End();
	}
}
#endif
