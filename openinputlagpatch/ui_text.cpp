#include <Windows.h>
#include <shlobj.h>
#include <shlwapi.h>
#include <stdio.h>
#include <string.h>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>
#include "config.h"
#include "ui_text.h"

namespace {
	std::vector<UiText::Entry> entries;
	unsigned int font_revision = 1;
#ifdef OILP_UI_TEXT_EDITOR
	wchar_t text_path[MAX_PATH];
	bool text_path_ready;
#endif

	int ClampLanguage(int language) {
		if (language < 0 || language > 2)
			return 0;
		return language;
	}

#ifdef OILP_UI_TEXT_EDITOR
	bool BuildPath() {
		if (text_path_ready)
			return text_path[0] != L'\0';
		text_path_ready = true;

		wchar_t appdata[MAX_PATH] = {};
		if (FAILED(SHGetFolderPathW(nullptr, CSIDL_APPDATA, nullptr, 0, appdata)))
			return false;
		wcscpy_s(text_path, appdata);
		PathAppendW(text_path, L"ShanghaiAlice");
		CreateDirectoryW(text_path, nullptr);
		PathAppendW(text_path, L"oilp_custom");
		CreateDirectoryW(text_path, nullptr);
		PathAppendW(text_path, L"ui_text.ini");
		return true;
	}

	std::string Trim(const std::string& text) {
		size_t start = 0;
		while (start < text.size() && (text[start] == ' ' || text[start] == '\t'))
			++start;
		size_t end = text.size();
		while (end > start && (text[end - 1] == ' ' || text[end - 1] == '\t'))
			--end;
		return text.substr(start, end - start);
	}

	std::string Unescape(const std::string& text) {
		std::string out;
		out.reserve(text.size());
		for (size_t i = 0; i < text.size(); ++i) {
			if (text[i] != '\\' || i + 1 >= text.size()) {
				out.push_back(text[i]);
				continue;
			}
			const char next = text[++i];
			switch (next) {
			case 'n': out.push_back('\n'); break;
			case 'r': out.push_back('\r'); break;
			case 't': out.push_back('\t'); break;
			case '\\': out.push_back('\\'); break;
			default:
				out.push_back(next);
				break;
			}
		}
		return out;
	}

	std::string Escape(const std::string& text) {
		std::string out;
		out.reserve(text.size());
		for (char c : text) {
			switch (c) {
			case '\\': out += "\\\\"; break;
			case '\n': out += "\\n"; break;
			case '\r': out += "\\r"; break;
			case '\t': out += "\\t"; break;
			default: out.push_back(c); break;
			}
		}
		return out;
	}
#endif

	size_t FindIndex(const char* id) {
		if (id == nullptr)
			return entries.size();
		for (size_t i = 0; i < entries.size(); ++i) {
			if (entries[i].id == id)
				return i;
		}
		return entries.size();
	}

	void AppendGlyphsFromText(ImVector<ImWchar>& out, const char* text) {
		if (text == nullptr)
			return;
		const unsigned char* p = (const unsigned char*)text;
		while (*p) {
			unsigned int codepoint = 0;
			if (*p < 0x80) {
				++p;
				continue;
			} else if ((*p & 0xE0) == 0xC0) {
				codepoint = ((*p & 0x1Fu) << 6) | (p[1] & 0x3Fu);
				p += 2;
			} else if ((*p & 0xF0) == 0xE0) {
				codepoint = ((*p & 0x0Fu) << 12) | ((p[1] & 0x3Fu) << 6) | (p[2] & 0x3Fu);
				p += 3;
			} else {
				++p;
				continue;
			}
			if (codepoint >= 0x7F && codepoint <= 0xFFFF) {
				out.push_back((ImWchar)codepoint);
				out.push_back((ImWchar)codepoint);
			}
		}
	}

	struct BakedText {
		const char* id;
		int language;
		const char* text;
	};

#ifndef OILP_UI_TEXT_EDITOR
#include "ui_text_baked.inc"
#endif
}

void UiText::Register(const char* id, const char* zh, const char* en, const char* ja,
	bool multiline) {
	if (id == nullptr || FindIndex(id) != entries.size())
		return;
	Entry entry;
	entry.id = id;
	entry.defaults[0] = zh != nullptr ? zh : "";
	entry.defaults[1] = en != nullptr ? en : "";
	entry.defaults[2] = ja != nullptr ? ja : "";
	entry.multiline = multiline;
	entries.push_back(std::move(entry));
}

const char* UiText::Get(const char* id) {
	return Get(id, Config::Language);
}

const char* UiText::Get(const char* id, int language) {
	const size_t index = FindIndex(id);
	if (index == entries.size())
		return nullptr;
	return Get(index, language);
}

const char* UiText::Get(size_t index, int language) {
	if (index >= entries.size())
		return nullptr;
	const int lang = ClampLanguage(language);
	const std::string& override = entries[index].overrides[lang];
	return (override.empty() ? entries[index].defaults[lang] : override).c_str();
}

const char* UiText::Default(size_t index, int language) {
	if (index >= entries.size())
		return nullptr;
	return entries[index].defaults[ClampLanguage(language)].c_str();
}

bool UiText::Find(const char* id, size_t* index) {
	const size_t found = FindIndex(id);
	if (found == entries.size())
		return false;
	if (index != nullptr)
		*index = found;
	return true;
}

void UiText::SetOverride(size_t index, int language, const char* text) {
	if (index >= entries.size())
		return;
	const int lang = ClampLanguage(language);
	if (text == nullptr || text[0] == '\0' || entries[index].defaults[lang] == text)
		entries[index].overrides[lang].clear();
	else
		entries[index].overrides[lang] = text;
}

void UiText::ResetOverride(size_t index, int language) {
	if (index >= entries.size())
		return;
	entries[index].overrides[ClampLanguage(language)].clear();
}

void UiText::ResetAllOverrides() {
	for (Entry& entry : entries) {
		for (std::string& text : entry.overrides)
			text.clear();
	}
}

bool UiText::HasOverride(size_t index, int language) {
	if (index >= entries.size())
		return false;
	return !entries[index].overrides[ClampLanguage(language)].empty();
}

#ifdef OILP_UI_TEXT_EDITOR
bool UiText::Load() {
	if (!BuildPath())
		return false;

	std::ifstream file(text_path, std::ios::binary);
	if (!file)
		return false;
	std::stringstream buffer;
	buffer << file.rdbuf();
	std::string content = buffer.str();
	if (content.size() >= 3 &&
		(unsigned char)content[0] == 0xEF &&
		(unsigned char)content[1] == 0xBB &&
		(unsigned char)content[2] == 0xBF)
		content.erase(0, 3);

	ResetAllOverrides();
	int language = -1;
	size_t start = 0;
	while (start <= content.size()) {
		size_t end = content.find('\n', start);
		if (end == std::string::npos)
			end = content.size();
		std::string line = content.substr(start, end - start);
		if (!line.empty() && line.back() == '\r')
			line.pop_back();
		start = end + 1;

		const std::string trimmed = Trim(line);
		if (trimmed.empty() || trimmed[0] == ';' || trimmed[0] == '#')
			continue;
		if (trimmed[0] == '[') {
			if (trimmed == "[zh]")
				language = 0;
			else if (trimmed == "[en]")
				language = 1;
			else if (trimmed == "[ja]")
				language = 2;
			else
				language = -1;
			continue;
		}
		if (language < 0)
			continue;

		const size_t equals = line.find('=');
		if (equals == std::string::npos)
			continue;
		const std::string key = Trim(line.substr(0, equals));
		const std::string value = Unescape(Trim(line.substr(equals + 1)));
		const size_t index = FindIndex(key.c_str());
		if (index == entries.size())
			continue;
		if (value.empty())
			entries[index].overrides[language].clear();
		else
			entries[index].overrides[language] = value;
	}
	RequestFontRebuild();
	return true;
}

bool UiText::Save() {
	if (!BuildPath())
		return false;
	std::ofstream file(text_path, std::ios::binary | std::ios::trunc);
	if (!file)
		return false;

	file << "\xEF\xBB\xBF";
	file << "; OpenInputLagPatch overlay text. Empty values use the built-in text.\r\n";
	file << "; Sections are [zh], [en] and [ja]. Newlines are written as \\n.\r\n";
	static const char* sections[] = { "zh", "en", "ja" };
	for (int language = 0; language < 3; ++language) {
		file << "\r\n[" << sections[language] << "]\r\n";
		for (const Entry& entry : entries) {
			const std::string& text = entry.overrides[language];
			if (!text.empty())
				file << entry.id << '=' << Escape(text) << "\r\n";
		}
	}
	return file.good();
}

const wchar_t* UiText::FilePath() {
	BuildPath();
	return text_path;
}
#endif

void UiText::ApplyBaked() {
#ifndef OILP_UI_TEXT_EDITOR
	for (const BakedText& baked : kBakedTexts) {
		if (baked.id == nullptr)
			break;
		const size_t index = FindIndex(baked.id);
		if (index == entries.size() || baked.language < 0 || baked.language > 2)
			continue;
		entries[index].overrides[baked.language] = baked.text != nullptr ? baked.text : "";
	}
#endif
}

const char* UiText::LanguageLabel(int language) {
	if (language == 0) {
		if (Config::Language == 2)
			return "簡体字中国語";
		if (Config::Language == 1)
			return "Simplified Chinese";
		return "简体中文";
	}
	if (language == 1)
		return "English";
	return "日本語";
}

void UiText::AppendGlyphs(ImVector<ImWchar>& out, int language) {
	const int lang = ClampLanguage(language);
	for (const Entry& entry : entries)
		AppendGlyphsFromText(out, Get(entry.id.c_str(), lang));
}

void UiText::RequestFontRebuild() {
	++font_revision;
}

unsigned int UiText::FontRevision() {
	return font_revision;
}

const std::vector<UiText::Entry>& UiText::Entries() {
	return entries;
}
