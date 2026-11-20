#include <Windows.h>
#include <stdio.h>
#include <wchar.h>
#include "common.h"
#include "sha256.h"

namespace {
struct UiMessageText {
	const wchar_t* english;
	const wchar_t* chinese;
	const wchar_t* japanese;
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
	{ L"Unable to get the game's path.", L"无法获取游戏程序路径。", L"ゲームのパスを取得できません。" },
	{ L"Failed to hash the game.", L"无法计算游戏程序的哈希值。", L"ゲームのハッシュ値を計算できません。" },
	{
		L"Failed to detect the game. Please make sure that the game you're trying to play is supported.\n"
		L"If you believe this is a mistake, please open an issue on Github.",
		L"无法识别当前游戏。请确认正在运行的游戏版本受支持。\n"
		L"如果确认版本无误，请在 GitHub 提交 issue。",
		L"ゲームを判別できません。実行中のゲームが対応しているか確認してください。\n"
		L"誤りだと思われる場合は GitHub で issue を作成してください。"
	},
	{
		L"The game hash was not recognized. The game was identified from its executable name as \"%s\".\n\n"
		L"OpenInputLagPatch patches may behave incorrectly or fail to start.",
		L"未能通过哈希确认当前游戏，已根据可执行文件名判断作品为\"%s\"。\n\n"
		L"当前游戏可能不是原版，OpenInputLagPatch 补丁可能出现异常或无法正常启动。",
		L"ゲームのハッシュ値を確認できませんでした。実行ファイル名から「%s」と判別しました。\n\n"
		L"OpenInputLagPatch のパッチが正しく動作しない、または起動に失敗する可能性があります。"
	},
	{
		L"vpatch and OpenInputLagPatch are incompatible.\n"
		L"Stop running the game via vpatch.exe or uninstall OpenInputLagPatch by deleting dinput8.dll.",
		L"vpatch 与 OpenInputLagPatch 不兼容。\n"
		L"请不要通过 vpatch.exe 启动游戏，或删除 dinput8.dll 以卸载 OpenInputLagPatch。",
		L"vpatch と OpenInputLagPatch は互換性がありません。\n"
		L"vpatch.exe 経由での起動をやめるか、dinput8.dll を削除して OpenInputLagPatch をアンインストールしてください。"
	},
	{
		L"Waiting...\n"
		L"If you don't want to see this, set DebugWait to 0 in the config.",
		L"等待中……\n"
		L"如果不希望看到此提示，请在配置中将 DebugWait 设为 0。",
		L"待機中……\n"
		L"この表示を消すには、設定で DebugWait を 0 にしてください。"
	},
	{
		L"Launch the game in fullscreen mode?\n\n"
		L"Yes: fullscreen, No: window mode",
		L"是否以全屏模式启动游戏？\n\n"
		L"“是”：全屏模式\n"
		L"“否”：窗口模式",
		L"ゲームを全画面モードで起動しますか？\n\n"
		L"「はい」：全画面モード\n"
		L"「いいえ」：ウィンドウモード"
	},
	{
		L"The game was detected, but the per-game patch function couldn't be found.",
		L"已识别当前游戏，但找不到对应的补丁函数。",
		L"ゲームを判別しましたが、対応するパッチ関数が見つかりません。"
	},
	{ L"Invalid target refresh rate %d", L"目标刷新率无效：%d", L"対象リフレッシュレートが無効です：%d" },
	{ L"Failed to create waitable timer. GLE: 0x%x", L"创建可等待计时器失败。GetLastError：0x%x", L"待機可能タイマーの作成に失敗しました。GetLastError：0x%x" },
	{ L"Tried to tick the limiter before initialization.", L"限帧器尚未初始化。", L"リミッターの初期化前に処理しようとしました。" },
	{ L"Temporary font texture CreateTexture failed! Error: 0x%x", L"创建临时字体纹理失败！错误码：0x%x", L"一時フォントテクスチャの作成に失敗しました！エラーコード：0x%x" },
	{ L"Temporary font texture LockRect failed! Error: 0x%x", L"锁定临时字体纹理失败！错误码：0x%x", L"一時フォントテクスチャのロックに失敗しました！エラーコード：0x%x" },
	{ L"Font texture CreateTexture failed! Error: 0x%x", L"创建字体纹理失败！错误码：0x%x", L"フォントテクスチャの作成に失敗しました！エラーコード：0x%x" },
	{ L"CreateVertexBuffer failed! Error: 0x%x", L"创建顶点缓冲区失败！错误码：0x%x", L"頂点バッファの作成に失敗しました！エラーコード：0x%x" },
	{ L"CreateIndexBuffer failed! Error: 0x%x", L"创建索引缓冲区失败！错误码：0x%x", L"インデックスバッファの作成に失敗しました！エラーコード：0x%x" },
	{ L"Tried to draw too many characters", L"尝试绘制的字符数量过多。", L"描画する文字数が多すぎます。" },
	{ L"Reset failed!\nCode: 0x%x", L"重置 D3D9 设备失败！\n错误码：0x%x", L"D3D9 デバイスのリセットに失敗しました！\nエラーコード：0x%x" },
	{ L"TestCooperativeLevel failed!\nCode: 0x%x", L"调用 TestCooperativeLevel 失败！\n错误码：0x%x", L"TestCooperativeLevel の呼び出しに失敗しました！\nエラーコード：0x%x" },
	{ L"CreateDeviceEx failed!\nCode: 0x%x", L"调用 CreateDeviceEx 失败！\n错误码：0x%x", L"CreateDeviceEx の呼び出しに失敗しました！\nエラーコード：0x%x" },
	{ L"CreateDevice failed!\nCode: 0x%x", L"调用 CreateDevice 失败！\n错误码：0x%x", L"CreateDevice の呼び出しに失敗しました！\nエラーコード：0x%x" },
	{ L"Couldn't find Direct3DCreate9Ex!", L"未找到 Direct3DCreate9Ex！", L"Direct3DCreate9Ex が見つかりません！" },
	{ L"Direct3DCreate9Ex failed!\nCode: 0x%x", L"调用 Direct3DCreate9Ex 失败！\n错误码：0x%x", L"Direct3DCreate9Ex の呼び出しに失敗しました！\nエラーコード：0x%x" },
	{ L"Couldn't find Direct3DCreate9!", L"未找到 Direct3DCreate9！", L"Direct3DCreate9 が見つかりません！" },
	{ L"Direct3DCreate9 failed!", L"调用 Direct3DCreate9 失败！", L"Direct3DCreate9 の呼び出しに失敗しました！" },
	{
		L"The resolution the game asks for is not offered for exclusive fullscreen on this system, so the game was started in window mode.\n\n"
		L"To use that resolution in exclusive fullscreen, change the graphics driver settings or add the resolution with Scaled Resolution Editor.",
		L"当前系统不支持该补丁独占全屏时显示已选择的游戏分辨率，已自动转为窗口模式启动游戏。\n\n"
		L"若要支持独占全屏时使用该游戏分辨率，请更改显卡设置或使用 Scaled Resolution Editor 来添加系统支持的分辨率。",
		L"このシステムでは、排他全画面時に選択したゲーム解像度を表示できません。自動的にウィンドウモードで起動しました。\n\n"
		L"排他全画面でその解像度を使うには、グラフィック設定を変更するか、Scaled Resolution Editor で解像度を追加してください。"
	},
};

static_assert(sizeof(ui_message_texts) / sizeof(ui_message_texts[0]) == UiMessage_Count,
	"Every UiMessage needs one localized string per language.");

enum UiLanguage {
	UiLanguage_English,
	UiLanguage_Chinese,
	UiLanguage_Japanese,
};

// The language the message boxes use. The user's display language comes first, then the system
// display language, then the system locale: some compatibility setups expose a different user UI
// language to the game process.
UiLanguage ui_language() {
	const WORD candidates[] = {
		PRIMARYLANGID(GetUserDefaultUILanguage()),
		PRIMARYLANGID(GetSystemDefaultUILanguage()),
		PRIMARYLANGID(GetSystemDefaultLangID()),
	};
	for (WORD language : candidates) {
		if (language == LANG_CHINESE)
			return UiLanguage_Chinese;
		if (language == LANG_JAPANESE)
			return UiLanguage_Japanese;
	}
	return UiLanguage_English;
}

HHOOK yes_no_close_hook;

void fit_yes_no_message_box(HWND hwnd) {
	HWND yes = GetDlgItem(hwnd, IDYES);
	HWND no = GetDlgItem(hwnd, IDNO);
	HWND cancel = GetDlgItem(hwnd, IDCANCEL);
	if (yes == nullptr || no == nullptr || cancel == nullptr)
		return;

	RECT yes_rect = {};
	RECT no_rect = {};
	GetWindowRect(yes, &yes_rect);
	GetWindowRect(no, &no_rect);
	MapWindowPoints(nullptr, hwnd, (POINT*)&yes_rect, 2);
	MapWindowPoints(nullptr, hwnd, (POINT*)&no_rect, 2);

	const int button_width = yes_rect.right - yes_rect.left;
	const int button_height = yes_rect.bottom - yes_rect.top;
	const int button_gap = no_rect.left - yes_rect.right;
	const int buttons_width = button_width * 2 + button_gap;
	const int desired_client_width = yes_rect.left * 2 + buttons_width;

	RECT client_rect = {};
	RECT window_rect = {};
	GetClientRect(hwnd, &client_rect);
	GetWindowRect(hwnd, &window_rect);
	const int old_window_width = window_rect.right - window_rect.left;
	const int old_window_height = window_rect.bottom - window_rect.top;
	const int nonclient_width = old_window_width - client_rect.right;
	const int new_window_width = desired_client_width + nonclient_width;
	const int window_x = window_rect.left + (old_window_width - new_window_width) / 2;

	ShowWindow(cancel, SW_HIDE);
	SetWindowPos(hwnd, nullptr, window_x, window_rect.top, new_window_width,
		old_window_height, SWP_NOZORDER | SWP_NOACTIVATE);

	const int button_x = (desired_client_width - buttons_width) / 2;
	SetWindowPos(yes, nullptr, button_x, yes_rect.top, 0, 0,
		SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
	SetWindowPos(no, nullptr, button_x + button_width + button_gap, no_rect.top, 0, 0,
		SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
}

LRESULT CALLBACK yes_no_close_cbt_proc(int code, WPARAM wparam, LPARAM lparam) {
	if (code == HCBT_ACTIVATE) {
		HWND hwnd = (HWND)wparam;
		wchar_t title[64] = {};
		GetWindowTextW(hwnd, title, sizeof(title) / sizeof(title[0]));
		if (wcscmp(title, ui_message_box_title) == 0 &&
			GetDlgItem(hwnd, IDYES) != nullptr &&
			GetDlgItem(hwnd, IDNO) != nullptr &&
			GetDlgItem(hwnd, IDCANCEL) != nullptr) {
			fit_yes_no_message_box(hwnd);
			if (yes_no_close_hook != nullptr) {
				UnhookWindowsHookEx(yes_no_close_hook);
				yes_no_close_hook = nullptr;
			}
		}
	}
	return CallNextHookEx(nullptr, code, wparam, lparam);
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

// The game keeps the cursor clipped to its own window and usually hides it. A message box sits
// outside that window, so the pointer is released and shown for as long as one is up; the count is
// put back afterwards so the game's own bookkeeping still adds up.
struct MessageCursorRelease {
	int shown = 0;
	MessageCursorRelease() {
		ClipCursor(nullptr);
		while (ShowCursor(TRUE) < 0)
			++shown;
		SetCursor(LoadCursorW(nullptr, IDC_ARROW));
	}
	~MessageCursorRelease() {
		for (int i = 0; i < shown; ++i)
			ShowCursor(FALSE);
	}
};

const wchar_t* ui_message_text(UiMessage message) {
	if ((unsigned int)message >= UiMessage_Count)
		return L"Unknown OpenInputLagPatch message.";

	const UiMessageText& text = ui_message_texts[(unsigned int)message];
	switch (ui_language()) {
	case UiLanguage_Chinese:
		return text.chinese;
	case UiLanguage_Japanese:
		return text.japanese;
	default:
		return text.english;
	}
}

int show_ui_message(UiMessage message, unsigned int type, ...) {
	wchar_t buf[1024] = {};
	va_list args;
	va_start(args, type);
	vswprintf_s(buf, ui_message_text(message), args);
	va_end(args);

	MessageCursorRelease cursor;
	return MessageBoxW(NULL, buf, ui_message_box_title, type);
}

int show_ui_yes_no_with_close(UiMessage message) {
	MessageCursorRelease cursor;
	yes_no_close_hook = SetWindowsHookExW(
		WH_CBT, yes_no_close_cbt_proc, nullptr, GetCurrentThreadId());
	const int answer = MessageBoxW(nullptr, ui_message_text(message), ui_message_box_title,
		MB_YESNOCANCEL | MB_DEFBUTTON1 | MB_ICONQUESTION | MB_SETFOREGROUND);
	if (yes_no_close_hook != nullptr) {
		UnhookWindowsHookEx(yes_no_close_hook);
		yes_no_close_hook = nullptr;
	}
	return answer;
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

		MessageCursorRelease cursor;
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

	MessageCursorRelease cursor;
	MessageBoxW(NULL, buf, ui_message_box_title, MB_ICONERROR);
	exit(1);
}
