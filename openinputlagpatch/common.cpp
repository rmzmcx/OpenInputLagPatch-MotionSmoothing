#include <Windows.h>
#include <stdio.h>
#include <wchar.h>
#include "common.h"
#include "sha256.h"

namespace {
struct UiMessageText {
	const wchar_t* english;
	const wchar_t* chinese;
};

const wchar_t ui_message_box_title[] = L"OpenInputLagPatch";
const unsigned int ui_message_queue_max = 4;

struct QueuedUiMessage {
	wchar_t text[1024];
	unsigned int type;
	bool exit_after;
};

QueuedUiMessage ui_message_queue[ui_message_queue_max];
unsigned int ui_message_queue_count;
volatile LONG showing_ui_message_queue;

const UiMessageText ui_message_texts[] = {
	{ L"Unable to get the game's path.", L"无法获取游戏程序路径。" },
	{ L"Failed to hash the game.", L"无法计算游戏程序的哈希值。" },
	{
		L"Failed to detect the game. Please make sure that the game you're trying to play is supported.\n"
		L"If you believe this is a mistake, please open an issue on Github.",
		L"无法识别当前游戏。请确认正在运行的游戏版本受支持。\n"
		L"如果确认版本无误，请在 GitHub 提交 issue。"
	},
	{
		L"The game hash was not recognized. The game was identified from its executable name as \"%s\".\n\n"
		L"OpenInputLagPatch patches may behave incorrectly or fail to start.",
		L"未能通过哈希确认当前游戏，已根据可执行文件名判断作品为\"%s\"。\n\n"
		L"当前游戏可能不是原版，OpenInputLagPatch 补丁可能出现异常或无法正常启动。"
	},
	{
		L"vpatch and OpenInputLagPatch are incompatible.\n"
		L"Stop running the game via vpatch.exe or uninstall OpenInputLagPatch by deleting dinput8.dll.",
		L"vpatch 与 OpenInputLagPatch 不兼容。\n"
		L"请不要通过 vpatch.exe 启动游戏，或删除 dinput8.dll 以卸载 OpenInputLagPatch。"
	},
	{
		L"Waiting...\n"
		L"If you don't want to see this, set DebugWait to 0 in the config.",
		L"等待中……\n"
		L"如果不希望看到此提示，请在配置中将 DebugWait 设为 0。"
	},
	{
		L"Launch the game in fullscreen mode?\n\n"
		L"Yes: fullscreen, No: window mode",
		L"是否以全屏模式启动游戏？\n\n"
		L"“是”：全屏模式\n"
		L"“否”：窗口模式"
	},
	{
		L"The game was detected, but the per-game patch function couldn't be found.",
		L"已识别当前游戏，但找不到对应的补丁函数。"
	},
	{ L"Invalid target refresh rate %d", L"目标刷新率无效：%d" },
	{ L"Failed to create waitable timer. GLE: 0x%x", L"创建可等待计时器失败。GetLastError：0x%x" },
	{ L"Tried to tick the limiter before initialization.", L"限帧器尚未初始化。" },
	{ L"Temporary font texture CreateTexture failed! Error: 0x%x", L"创建临时字体纹理失败！错误码：0x%x" },
	{ L"Temporary font texture LockRect failed! Error: 0x%x", L"锁定临时字体纹理失败！错误码：0x%x" },
	{ L"Font texture CreateTexture failed! Error: 0x%x", L"创建字体纹理失败！错误码：0x%x" },
	{ L"CreateVertexBuffer failed! Error: 0x%x", L"创建顶点缓冲区失败！错误码：0x%x" },
	{ L"CreateIndexBuffer failed! Error: 0x%x", L"创建索引缓冲区失败！错误码：0x%x" },
	{ L"Tried to draw too many characters", L"尝试绘制的字符数量过多。" },
	{ L"Reset failed!\nCode: 0x%x", L"重置 D3D9 设备失败！\n错误码：0x%x" },
	{ L"TestCooperativeLevel failed!\nCode: 0x%x", L"调用 TestCooperativeLevel 失败！\n错误码：0x%x" },
	{ L"CreateDeviceEx failed!\nCode: 0x%x", L"调用 CreateDeviceEx 失败！\n错误码：0x%x" },
	{ L"CreateDevice failed!\nCode: 0x%x", L"调用 CreateDevice 失败！\n错误码：0x%x" },
	{ L"Couldn't find Direct3DCreate9Ex!", L"未找到 Direct3DCreate9Ex！" },
	{ L"Direct3DCreate9Ex failed!\nCode: 0x%x", L"调用 Direct3DCreate9Ex 失败！\n错误码：0x%x" },
	{ L"Couldn't find Direct3DCreate9!", L"未找到 Direct3DCreate9！" },
	{ L"Direct3DCreate9 failed!", L"调用 Direct3DCreate9 失败！" },
};

static_assert(sizeof(ui_message_texts) / sizeof(ui_message_texts[0]) == UiMessage_Count,
	"Every UiMessage needs one localized string pair.");

bool chinese_ui_is_active() {
	// Prefer the Windows display language, but also honour a Chinese system locale. Some
	// compatibility setups expose a different user UI language to the game process.
	return PRIMARYLANGID(GetSystemDefaultUILanguage()) == LANG_CHINESE ||
		PRIMARYLANGID(GetUserDefaultUILanguage()) == LANG_CHINESE ||
		PRIMARYLANGID(GetSystemDefaultLangID()) == LANG_CHINESE;
}
} // namespace

// Gets the SHA256 of a file
// Returns true on success
bool sha256_file(wchar_t* path, char* hash_str) {
	// Open it
	FILE* file = _wfopen(path, L"rb");
	if (!file) {
		return false;
	}

	// Read it into memory
	fseek(file, 0, SEEK_END);
	int size = ftell(file);
	fseek(file, 0, SEEK_SET);
	BYTE* data = new BYTE[size];
	if (fread(data, 1, size, file) != size) {
		return true;
	}
	fclose(file);

	// Hash it
	SHA256_CTX ctx = {};
	BYTE hash[SHA256_BLOCK_SIZE] = {};
	sha256_init(&ctx);
	sha256_update(&ctx, data, size);
	sha256_final(&ctx, hash);
	delete[] data;

	// Convert the hash to a string
	for (int i = 0; i < SHA256_BLOCK_SIZE; i++) {
		hash_str[i * 2] = "0123456789abcdef"[hash[i] >> 4];
		hash_str[i * 2 + 1] = "0123456789abcdef"[hash[i] & 0xF];
	}
	return true;
}

const wchar_t* ui_message_text(UiMessage message) {
	if ((unsigned int)message >= UiMessage_Count)
		return L"Unknown OpenInputLagPatch message.";

	const UiMessageText& text = ui_message_texts[(unsigned int)message];
	return chinese_ui_is_active() ? text.chinese : text.english;
}

int show_ui_message(UiMessage message, unsigned int type, ...) {
	wchar_t buf[1024] = {};
	va_list args;
	va_start(args, type);
	vswprintf_s(buf, ui_message_text(message), args);
	va_end(args);

	return MessageBoxW(NULL, buf, ui_message_box_title, type);
}

void queue_ui_message(UiMessage message, unsigned int type, bool exit_after, const wchar_t* argument) {
	if (ui_message_queue_count >= ui_message_queue_max)
		return;

	QueuedUiMessage& pending = ui_message_queue[ui_message_queue_count++];
	pending.type = type;
	pending.exit_after = exit_after;
	if (argument != nullptr)
		swprintf_s(pending.text, ui_message_text(message), argument);
	else
		wcscpy_s(pending.text, ui_message_text(message));
}

void show_queued_ui_messages() {
	if (InterlockedCompareExchange(&showing_ui_message_queue, 1, 0) != 0)
		return;

	while (ui_message_queue_count != 0) {
		const QueuedUiMessage pending = ui_message_queue[0];
		for (unsigned int i = 1; i < ui_message_queue_count; ++i)
			ui_message_queue[i - 1] = ui_message_queue[i];
		--ui_message_queue_count;

		MessageBoxW(NULL, pending.text, ui_message_box_title, pending.type);
		if (pending.exit_after)
			exit(1);
	}
	InterlockedExchange(&showing_ui_message_queue, 0);
}

void __declspec(noreturn) panic_msgbox(UiMessage message, ...) {
	wchar_t buf[1024] = {};
	va_list args;
	va_start(args, message);
	vswprintf_s(buf, ui_message_text(message), args);
	va_end(args);

	MessageBoxW(NULL, buf, ui_message_box_title, MB_ICONERROR);
	exit(1);
}
