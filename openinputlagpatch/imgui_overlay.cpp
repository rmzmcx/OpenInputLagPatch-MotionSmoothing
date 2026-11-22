#include <Windows.h>
#include <shlobj.h>
#include <shlwapi.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <fstream>
#include <string>
#include "config.h"
#include "limiter.h"
#include "overlay.h"
#include "imgui/imgui.h"
#include "imgui/imgui_impl_dx8.h"
#include "imgui/imgui_impl_dx9.h"
#include "imgui/imgui_impl_win32.h"
#include "imgui/imgui_freetype.h"
#include "imgui_overlay.h"
#include "ui_text.h"
#ifdef OILP_UI_TEXT_EDITOR
#include "ui_text_editor.h"
#endif

namespace OilpImgui {
	static bool installed = false;
	static bool open = false;
	static bool hotkey_down = false;
	static DWORD hint_until = 0;

	// Temporary: a trace of what the overlay does, written next to the game's exe.
	static FILE* trace_file = nullptr;
	static void Trace(const char* format, ...) {
		if (!trace_file) {
			char path[MAX_PATH] = {};
			GetModuleFileNameA(nullptr, path, MAX_PATH);
			char* dot = strrchr(path, '.');
			if (dot)
				strcpy_s(dot, (size_t)(MAX_PATH - (dot - path)), ".imgui.log");
			if (fopen_s(&trace_file, path, "a") != 0)
				return;
		}
		va_list args;
		va_start(args, format);
		vfprintf(trace_file, format, args);
		va_end(args);
		fflush(trace_file);
	}

	// The fixed part of the panels: tab names and the few sentences that do not belong to one ini
	// key. Everything that does is in the tables below.
	struct Strings {
		const char* keys; const char* info; const char* config;
		const char* overlay_key; const char* diag_key; const char* language;
		const char* restart; const char* hint; const char* capture_hint; const char* free_adjust;
		const char* info_logic; const char* info_present; const char* info_shown;
	};
	static const Strings kStrings[3] = {
		{
			"按键", "信息", "配置",
			"打开面板", "导出诊断", "语言", "重启后生效",
			"OpenInputLagPatch：Ctrl+Shift+O 打开补丁配置",
			"请按下要使用的按键",
			"自由调整",
			"逻辑帧率", "呈现帧率", "实际呈现",
		},
		{
			"Keys", "Info", "Config",
			"Overlay key", "Diag key", "Language", "restart to apply",
			"OpenInputLagPatch: Ctrl+Shift+O to open the patch settings",
			"Press the keys to use",
			"Free adjust",
			"Logic frame rate", "Presentation rate", "Presented",
		},
		{
			"キー", "情報", "設定",
			"オーバーレイキー", "診断出力", "言語", "再起動後に反映",
			"OpenInputLagPatch：Ctrl+Shift+O で設定を開く",
			"使用するキーを押してください",
			"自由調整",
			"論理フレームレート", "提示フレームレート", "実際の提示",
		},
	};

	static const char* T(const char* id) {
		const char* text = UiText::Get(id);
		return text != nullptr ? text : id;
	}

	static Strings S() {
		return {
			T("tab.keys"), T("tab.info"), T("tab.config"),
			T("keys.overlay"), T("keys.diag"), T("keys.language"), T("keys.restart"),
			T("hint.startup"), T("keys.capture_hint"), T("label.free_adjust"),
			T("info.logic"), T("info.present"), T("info.shown"),
		};
	}

	// The name each ini key is shown under. The key itself stays the label the code passes around,
	// so the panels can be renamed without touching what is written back.
	struct OptionName {
		const char* key;
		const char* en;
		const char* zh;
		const char* ja;
	};

	static const OptionName kOptionName[] = {
		{ "UseInGameImguiOverlay", "In-game settings", "游戏内设置界面", "ゲーム内設定画面" },
		{ "ShowPatchAppliedMessage", "Startup notice", "启动提示", "起動時の通知" },

		{ "Interpolation", "Interpolation", "补帧", "フレーム補間" },
		{ "FixBulletPixelSnapping", "Bullet subpixel alignment", "子弹像素对齐", "弾のサブピクセル整列" },
		{ "PlayerBody", "Player body", "自机本体", "自機本体" },
		{ "PlayerShots", "Player shots", "自机射击的子弹", "自機ショット" },
		{ "BulletsAndLasers", "Enemy bullets and lasers", "敌弹与直线激光", "敵弾と直線レーザー" },

		{ "GameFPS", "Game logic frame rate", "游戏逻辑帧率", "ゲーム論理フレームレート" },
		{ "ReplaySpeedControl", "Replay speed control", "replay 变速", "リプレイ速度調整" },
		{ "ReplaySkipFPS", "Replay fast-forward rate", "replay 快进帧率", "リプレイ早送りレート" },
		{ "ReplaySlowFPS", "Replay slow rate", "replay 慢放帧率", "リプレイスローレート" },
		{ "BltPrepareTime", "Presentation headroom", "呈现预留时间", "提示の余裕時間" },
		{ "Sleep", "Wait method", "等待方式", "待機方法" },
		{ "D3D9Ex", "D3D9Ex device", "D3D9Ex 设备", "D3D9Ex デバイス" },
		{ "FullscreenRefreshRate", "Fullscreen refresh rate", "全屏刷新率", "全画面リフレッシュレート" },
		{ "ForceRatio_4_3", "Force 4:3", "强制画面 4:3", "4:3 を強制" },
		{ "ShowOverlay", "Performance readout", "显示性能数据", "パフォーマンス表示" },
		{ "DebugConsole", "Debug console", "调试控制台", "デバッグコンソール" },
		{ "DebugWait", "Wait for debugger", "启动等待调试器", "デバッガ待機" },
		{ "FixTH07Cherry&BonusDisplay", "Fix th07 cherry and spell bonus", "修复妖妖梦的樱点和符卡分显示", "th07 の桜点とスペルボーナス表示を修正" },
		{ "FixTH10ReplayResave", "Fix th10 replay re-save", "修复风神录重复保存 Replay 出错的问题", "th10 のリプレイ再保存を修正" },
		{ "FixTH12ScoreDisplay", "Fix th12 score display", "修复星莲船分数显示错误的问题", "th12 のスコア表示を修正" },
		{ "FixPracModeReplayCrash", "Fix practice mode replay crash", "修复练习模式对话结束后崩溃的问题", "練習モードのリプレイクラッシュを修正" },
		{ "FixInputGlitching", "Fix stuck input", "修复红魔乡和妖妖梦的卡键问题", "th06 と th07 の入力固着を修正" },
		{ "CalcFPS", "Take over the frame rate measurement", "接管帧率统计", "フレームレート計測を引き継ぐ" },
		{ "AlwaysBlt", "Keep rendering while inactive", "画面失焦后继续渲染", "非アクティブ時も描画を続ける" },
		{ "GameOverride", "Force a specific game", "强制让补丁使用特定作品的代码", "特定の作品として扱う" },

		{ "AskWindowMode", "Ask for fullscreen on startup", "启动时选择是否全屏", "起動時に全画面を尋ねる" },
		{ "enabled", "Window settings", "窗口设置开关", "ウィンドウ設定" },
		{ "X", "Window x", "窗口 x 坐标", "ウィンドウ x 座標" },
		{ "Y", "Window y", "窗口 y 坐标", "ウィンドウ y 座標" },
		{ "Width", "Window width", "窗口宽度", "ウィンドウ幅" },
		{ "Height", "Window height", "窗口高度", "ウィンドウ高さ" },
		{ "TitleBar", "Title bar", "显示标题栏", "タイトルバーを表示" },
		{ "AlwaysOnTop", "Always on top", "窗口置顶", "常に最前面" },
		{ "CoverTaskbar", "Cover the taskbar", "是否覆盖任务栏", "タスクバーを覆う" },

		{ "BypassNvidiaD3D8Detour", "NVIDIA Detours workaround", "NVIDIA 挂钩绕过", "NVIDIA フックの回避" },
	};

	static const char* OptionDisplayName(const char* key) {
		char id[192] = {};
		sprintf_s(id, "option.%s.name", key);
		const char* text = UiText::Get(id);
		return text != nullptr ? text : key;
	}

	// The description behind the (?) marker, also per language. Keys that are left out simply have
	// no marker.
	struct OptionHelp {
		const char* key;
		const char* en;
		const char* zh;
		const char* ja;
	};

	static const OptionHelp kOptionHelp[] = {
		{ "UseInGameImguiOverlay", "0 turns the in-game settings window off completely.", "关闭后游戏内设置界面完全不可用。", "0 でゲーム内設定画面を完全に無効にします。" },
		{ "ShowPatchAppliedMessage", "Whether the short notice that the patch is running is shown on startup.", "启动时是否在右下角显示补丁已生效的提示。", "起動時にパッチが動作中であることを右下に表示するかどうか。" },

		{ "Interpolation", "The extra frames are extrapolated, so no render latency is added.", "补帧使用外推法，不增加渲染延迟。", "補間は外挿で行うため描画遅延は増えません。" },
		{ "FixBulletPixelSnapping", "Keeps the real subpixel position of bullets instead of rounding it to whole pixels.\nThis changes how the original looks even with the interpolation off.", "保留子弹的真实亚像素位置，不再四舍五入到整像素。\n这是更改了原版的特性，即使不开补帧也会有明显区别。", "弾の実際のサブピクセル位置を保持し、整数ピクセルに丸めません。\n補間を切っていても元の見た目から変わります。" },
		{ "PlayerBody", "Whether the extra frames extrapolate the player's body.", "补帧时是否外推自机本体。", "補間時に自機本体を外挿するかどうか。" },
		{ "PlayerShots", "Whether the extra frames extrapolate the player's shots.", "补帧时是否外推自机射击的子弹。", "補間時に自機ショットを外挿するかどうか。" },
		{ "BulletsAndLasers", "Whether the extra frames extrapolate enemy bullets and straight lasers.", "补帧时是否外推敌弹与直线激光。", "補間時に敵弾と直線レーザーを外挿するかどうか。" },

		{ "GameFPS", "Changes the game's logic frame rate, which changes its speed. 60 is the lowest.", "调整游戏的逻辑帧率，改变游戏速度，不能低于 60。", "ゲームの論理フレームレートを変更し、ゲーム速度が変わります。60 未満にはできません。" },
		{ "ReplaySpeedControl", "Whether a hotkey can fast-forward or slow down a replay.", "是否允许用快捷键让 replay 可以快进或慢放。", "ホットキーでリプレイを早送り、またはスロー再生できるようにします。" },
		{ "ReplaySkipFPS", "The frame rate used while a replay is fast-forwarded, held with the shot or dialog-skip key.", "replay 快进时使用的帧率，使用射击键或跳过对话键。", "リプレイ早送り中のフレームレート。ショットキーまたは会話スキップキーで行います。" },
		{ "ReplaySlowFPS", "The frame rate used while a replay is slowed down, held with the focus key.", "replay 慢放时使用的帧率，使用低速移动按键。", "リプレイスロー再生中のフレームレート。低速移動キーで行います。" },
		{ "BltPrepareTime", "How much of the frame is left for the present. Tuned with the render time readout (ShowOverlay = 2).", "为呈现预留的时间，配合渲染耗时显示（ShowOverlay = 2）调整。", "描画に残す時間。描画時間表示（ShowOverlay = 2）を見ながら調整します。" },
		{ "Sleep", "The high-precision wait spins without pause.\nThe other uses vpatch's own wait mechanism, which uses less CPU but is more likely to drop frames on some machines.", "高精度等待为不间断自旋；\n另一个是使用 vpatch 自身的等待机制，CPU 占用更低，但部分电脑上可能更容易跳帧。", "高精度待機は途切れずにスピンします。\nもう一方は vpatch 自身の待機機構を使うため CPU 負荷は低くなりますが、一部の環境ではフレーム落ちしやすくなります。" },
		{ "D3D9Ex", "Creates the device as D3D9Ex, which allows the frame queue and GPU priority tweaks under the BitBlt presentation model to cut latency.", "以 D3D9Ex 创建设备，在 BitBlt 显示模型下可调整帧队列与 GPU 优先级以降低延迟。", "D3D9Ex としてデバイスを作成します。BitBlt 表示モデルではフレームキューと GPU 優先度を調整でき、遅延を抑えられます。" },
		{ "FullscreenRefreshRate", "0 keeps the display's own rate, 1 forces 60hz, 2 picks the highest multiple of 60 the display offers at that resolution.", "显示器刷新率保持原样；60Hz 强制 60；60 的倍数取该分辨率下支持的最高 60 倍数。", "0 はディスプレイの設定のまま。1 は 60Hz 固定。2 はその解像度で選べる 60 の倍数の最大値。" },
		{ "ForceRatio_4_3", "Noticeably reduces image quality.", "会明显降低画质！", "画質が目に見えて落ちます。" },
		{ "DebugConsole", "Attaches a console window with the patch's own live log output.", "打开一个控制台窗口显示补丁自身的实时日志。", "パッチ自身のリアルタイムログを表示するコンソールを開きます。" },
		{ "DebugWait", "Shows a floating window on startup and waits for the debugger to attach.", "启动时显示一个悬浮窗，暂停等待调试器附加。", "起動時にフローティングウィンドウを表示し、デバッガの接続を待って停止します。" },
		{ "FixTH07Cherry&BonusDisplay", "Fixes the cherry point and spell bonus display in th07.", "修正 th07 的樱花点与符卡分显示。", "th07 の桜点とスペルボーナス表示を修正します。" },
		{ "FixTH10ReplayResave", "Keeps a th10 replay from recording a doubled frame count when it is saved again.", "避免 th10 的 replay 二次保存时记录成倍的帧数。", "th10 のリプレイを再保存したときにフレーム数が倍増して記録されるのを防ぎます。" },
		{ "FixTH12ScoreDisplay", "Fixes the score display in th12.", "修正 th12 的分数显示。", "th12 のスコア表示を修正します。" },
		{ "FixPracModeReplayCrash", "Only th12, th13, th14, th15 and th17 have this problem.", "仅风庙城绀天鬼有这个问题。", "この問題が起きるのは th12、th13、th14、th15、th17 のみです。" },
		{ "FixInputGlitching", "Usually shows up as a stuck key when the picture is switched.", "一般体现在切屏时会卡键。", "画面切り替え時に入力が固着する形で現れます。" },
		{ "CalcFPS", "Improves the frame rate measurement in th06, th07 and th08. The same feature as vpatch's CalcFPS.", "优化红魔乡、妖妖梦、永夜抄的帧率检测逻辑，和 vpatch 的 CalcFPS 是同一个功能。", "th06、th07、th08 のフレームレート計測を改善します。vpatch の CalcFPS と同じ機能です。" },
		{ "AlwaysBlt", "Lets the window of th06 and th07 keep rendering and presenting while it is inactive, so the picture does not freeze after alt-tabbing.", "让红魔乡和妖妖梦的窗口失去焦点时继续渲染与呈现，避免 Alt+Tab 后画面冻结。", "th06 と th07 のウィンドウが非アクティブでも描画と提示を続け、Alt+Tab 後に画面が固まるのを防ぎます。" },
		{ "GameOverride", "Usually leave this alone. The numbers each title uses are listed in the README on the GitHub page.", "一般不要乱动！具体哪些数字对应哪些作品可以在 GitHub 界面上的 README 看。", "通常は変更しないでください。各作品に対応する数値は GitHub の README に記載されています。" },

		{ "enabled", "Whether the window settings below are applied while the game runs in a window.", "游戏以窗口运行时是否应用下面的窗口设置。", "ウィンドウモードのときに以下の設定を適用するかどうか。" },
		{ "X", "Window x position in pixels. 2147483648 lets Windows choose.", "窗口 x 坐标；2147483648 为交给 Windows 决定。", "ウィンドウの x 座標（ピクセル）。2147483648 で Windows に任せます。" },
		{ "Y", "Window y position in pixels. 2147483648 lets Windows choose.", "窗口 y 坐标；2147483648 为交给 Windows 决定。", "ウィンドウの y 座標（ピクセル）。2147483648 で Windows に任せます。" },
		{ "Width", "Window width in pixels. 0 keeps the game's own size.", "窗口宽度；0 保持游戏自身尺寸。", "ウィンドウの幅（ピクセル）。0 でゲーム本来のサイズのまま。" },
		{ "Height", "Window height in pixels. 0 keeps the game's own size.", "窗口高度；0 保持游戏自身尺寸。", "ウィンドウの高さ（ピクセル）。0 でゲーム本来のサイズのまま。" },
		{ "AlwaysOnTop", "Holds the window above every other one.", "让窗口始终位于其他窗口之上。", "ウィンドウを常に最前面に表示します。" },
		{ "CoverTaskbar", "The two methods work differently. Marking the window as fullscreen declares it fullscreen to the shell; on top while active only raises it while it has focus.", "两个方法原理不同。「标记为全屏窗口」向系统声明全屏；「活动时置顶」仅在窗口在焦点时置顶。", "2 つの方法は仕組みが異なります。「全画面ウィンドウ扱い」はシェルに全画面として宣言し、「アクティブ時のみ最前面」はフォーカス中だけ最前面に置きます。" },

		{ "BypassNvidiaD3D8Detour", "Works around NVIDIA's broken Detours stub over dgVoodoo2 on some older Optimus laptops. Do not enable it unless something is actually broken.", "绕过部分老式 Optimus 笔记本上 NVIDIA 对 dgVoodoo2 的损坏挂钩，无异常情况请勿勾选。", "一部の古い Optimus 搭載ノートで NVIDIA が dgVoodoo2 に張る不具合のあるフックを回避します。問題がない場合は有効にしないでください。" },
	};

	static const char* OptionDescription(const char* key) {
		char id[192] = {};
		sprintf_s(id, "option.%s.help", key);
		return UiText::Get(id);
	}

	// The labels of the radio buttons, in the same three languages
	static const char* const kInterpolationForms[5][3] = {
		{ "Off", "关闭", "オフ" },
		{ "Display rate", "跟随刷新率", "リフレッシュレート" },
		{ "Largest 60fps multiple", "60 帧最大整数倍", "60fps の最大整数倍" },
		{ "Per frame", "每帧次数", "フレーム毎回数" },
		{ "Fixed target rate", "固定目标帧率", "固定目標レート" },
	};
	static const char* const kSleepChoices[2][3] = {
		{ "High-precision wait", "高精度等待", "高精度待機" },
		{ "vpatch's wait", "vpatch 的等待方式", "vpatch の待機方式" },
	};
	static const char* const kRefreshChoices[3][3] = {
		{ "Display rate", "显示器刷新率", "ディスプレイ" },
		{ "60hz", "60Hz", "60Hz" },
		{ "Multiple of 60", "60 的倍数", "60 の倍数" },
	};
	static const char* const kOverlayChoices[4][3] = {
		{ "Off", "不显示", "非表示" },
		{ "Frame rate only", "仅显示补帧实际呈现帧率", "補間の実提示レートのみ" },
		{ "Render time only", "仅显示游戏内部渲染耗时", "ゲーム内部の描画時間のみ" },
		{ "Both", "同时显示两者", "両方を表示" },
	};
	static const char* const kTaskbarChoices[3][3] = {
		{ "Off", "否", "いいえ" },
		{ "Mark as fullscreen window", "标记为全屏窗口", "全画面ウィンドウ扱い" },
		{ "On top while active", "活动时置顶", "アクティブ時のみ最前面" },
	};

	static void RegisterChoices(const char* group, const char* const (*choices)[3], int count) {
		for (int i = 0; i < count; ++i) {
			char id[160] = {};
			sprintf_s(id, "choice.%s.%d", group, i);
			UiText::Register(id, choices[i][1], choices[i][0], choices[i][2]);
		}
	}

	static void RegisterTexts() {
		static bool registered = false;
		if (registered)
			return;
		registered = true;

		UiText::Register("tab.keys", kStrings[0].keys, kStrings[1].keys, kStrings[2].keys);
		UiText::Register("tab.info", kStrings[0].info, kStrings[1].info, kStrings[2].info);
		UiText::Register("tab.config", kStrings[0].config, kStrings[1].config, kStrings[2].config);
		UiText::Register("keys.overlay", kStrings[0].overlay_key, kStrings[1].overlay_key, kStrings[2].overlay_key);
		UiText::Register("keys.diag", kStrings[0].diag_key, kStrings[1].diag_key, kStrings[2].diag_key);
		UiText::Register("keys.language", kStrings[0].language, kStrings[1].language, kStrings[2].language);
		UiText::Register("keys.restart", kStrings[0].restart, kStrings[1].restart, kStrings[2].restart);
		UiText::Register("hint.startup", kStrings[0].hint, kStrings[1].hint, kStrings[2].hint);
		UiText::Register("keys.capture_hint", kStrings[0].capture_hint, kStrings[1].capture_hint, kStrings[2].capture_hint);
		UiText::Register("label.free_adjust", kStrings[0].free_adjust, kStrings[1].free_adjust, kStrings[2].free_adjust);
		UiText::Register("info.logic", kStrings[0].info_logic, kStrings[1].info_logic, kStrings[2].info_logic);
		UiText::Register("info.present", kStrings[0].info_present, kStrings[1].info_present, kStrings[2].info_present);
		UiText::Register("info.shown", kStrings[0].info_shown, kStrings[1].info_shown, kStrings[2].info_shown);
		UiText::Register("button.apply", "应用", "Apply", "適用");

		for (const OptionName& name : kOptionName) {
			char id[192] = {};
			sprintf_s(id, "option.%s.name", name.key);
			UiText::Register(id, name.zh, name.en, name.ja);
		}
		for (const OptionHelp& help : kOptionHelp) {
			char id[192] = {};
			sprintf_s(id, "option.%s.help", help.key);
			UiText::Register(id, help.zh, help.en, help.ja, true);
		}

		RegisterChoices("Interpolation", kInterpolationForms, 5);
		RegisterChoices("Sleep", kSleepChoices, 2);
		RegisterChoices("FullscreenRefreshRate", kRefreshChoices, 3);
		RegisterChoices("ShowOverlay", kOverlayChoices, 4);
		RegisterChoices("CoverTaskbar", kTaskbarChoices, 3);
	}

	static void AppendLanguageLabels(ImVector<ImWchar>& range) {
		static const ImWchar labels[] = {
			0x7B80, 0x7B80, 0x4F53, 0x4F53, 0x4E2D, 0x4E2D, 0x6587, 0x6587,
			0x65E5, 0x65E5, 0x672C, 0x672C, 0x8A9E, 0x8A9E,
			0x7C21, 0x7C21, 0x5B57, 0x5B57, 0x56FD, 0x56FD, 0
		};
		for (const ImWchar* p = labels; *p; p++)
			range.push_back(*p);
	}

	// Every character the panels can draw, so the atlas holds them all. The common Chinese set
	// covers most of it; the editable text and the labels add the rest.
	static ImVector<ImWchar> chinese_range;
	static const ImWchar* ChineseRange() {
		ImFontAtlas* atlas = ImGui::GetIO().Fonts;
		chinese_range.clear();
		const ImWchar* common = atlas->GetGlyphRangesChineseSimplifiedCommon();
		for (const ImWchar* p = common; *p; p++)
			chinese_range.push_back(*p);
		UiText::AppendGlyphs(chinese_range, 0);
		AppendLanguageLabels(chinese_range);
		chinese_range.push_back(0);
		return chinese_range.Data;
	}

	// The Latin set plus the CJK of the language labels: the English font is the Chinese one,
	// which carries Latin as well, so the labels keep their own scripts.
	static ImVector<ImWchar> english_range;
	static const ImWchar* EnglishRange() {
		ImFontAtlas* atlas = ImGui::GetIO().Fonts;
		english_range.clear();
		const ImWchar* latin = atlas->GetGlyphRangesDefault();
		for (const ImWchar* p = latin; *p; p++)
			english_range.push_back(*p);
		UiText::AppendGlyphs(english_range, 1);
		AppendLanguageLabels(english_range);
		english_range.push_back(0);
		return english_range.Data;
	}

	// The Japanese set plus the simplified characters the language radio shows
	static ImVector<ImWchar> japanese_range;
	static const ImWchar* JapaneseRange() {
		ImFontAtlas* atlas = ImGui::GetIO().Fonts;
		japanese_range.clear();
		const ImWchar* japanese = atlas->GetGlyphRangesJapanese();
		for (const ImWchar* p = japanese; *p; p++)
			japanese_range.push_back(*p);
		UiText::AppendGlyphs(japanese_range, 2);
		AppendLanguageLabels(japanese_range);
		japanese_range.push_back(0);
		return japanese_range.Data;
	}

	// The panels are laid out for a 720p picture; larger windows get scaled up so the text and the
	// controls keep the same physical size on screen. 480p gets 80% of that.
	static float DisplayScale() {
		float scale = ImGui::GetIO().DisplaySize.y / 720.0f;
		if (scale < 0.8f)
			scale = 0.8f;
		if (scale > 2.0f)
			scale = 2.0f;
		return scale;
	}

	static float FontRasterSize() {
		// A REAL_DIM request is measured from ascender to descender, and the CJK faces stretch
		// that span to 1.3x their em, so the glyph lands at about 0.77 of the number asked for.
		// 16px is what thprac asks for on a 640x480 picture and it renders a 12x12 「中」; 13px
		// gave 10x9, which is where the hinter was dropping the bottom strokes.
		// The size tracks the picture so every resolution rasterises at 1:1. A fixed atlas drawn
		// through FontGlobalScale is what left 720p and 960p looking scaled up and soft.
		return 20.0f * DisplayScale();
	}

	// The system font for a language: whatever the OS has for the script. Falls back to the
	// built-in font when the file is missing.
	static ImFont* LoadLanguageFont(int language) {
		ImGuiIO& io = ImGui::GetIO();
		const char* paths[3] = {
			"C:\\Windows\\Fonts\\msyh.ttc",
			"C:\\Windows\\Fonts\\msyh.ttc",
			"C:\\Windows\\Fonts\\msgothic.ttc",
		};
		const ImWchar* ranges[3] = {
			ChineseRange(),
			EnglishRange(),
			JapaneseRange(),
		};
		if (language < 0 || language > 2)
			language = 0;
		io.Fonts->TexDesiredWidth = 4096;
		ImFontConfig cfg;
		// FreeType hints each glyph onto the pixel grid before the atlas is written, and that is
		// where the sharper strokes come from. Its rasteriser ignores OversampleH/V and
		// PixelSnapH, so only the brightness multiplier still applies; 1.25 is thprac's value.
		cfg.RasterizerMultiply = 1.25f;
		ImFont* font = io.Fonts->AddFontFromFileTTF(paths[language], FontRasterSize(), &cfg, ranges[language]);
		if (font == nullptr)
			font = io.Fonts->AddFontDefault();
		ImGuiFreeType::BuildFontAtlas(io.Fonts, 0);
		Trace("font: language=%d path=%s size=%.1f ascent=%.1f loaded=%d atlas=%dx%d\n",
			language, paths[language], FontRasterSize(), font != nullptr ? font->Ascent : 0.0f,
			font != nullptr, io.Fonts->TexWidth, io.Fonts->TexHeight);
		return font;
	}

	static int pending_language = -1;

	void SetLanguage(int language) {
		if (language < 0 || language > 2)
			return;
		Config::Language = language;
		pending_language = language;
	}

	static void ApplyPendingLanguage() {
		if (pending_language < 0)
			return;
		int language = pending_language;
		pending_language = -1;
		ImGuiIO& io = ImGui::GetIO();
		io.Fonts->Clear();
		// LoadLanguageFont builds the atlas through FreeType itself
		io.FontDefault = LoadLanguageFont(language);
		Gui::ImplDX9InvalidateDeviceObjects();
		Gui::ImplDX8InvalidateDeviceObjects();
	}

	bool Enabled() {
		// Read once and cached, the way the other restart-only options work: the value decides
		// the next launch, not this one.
		static const bool enabled = Config::UseInGameImguiOverlay != FALSE;
		return enabled;
	}

	bool IsOpen() {
		return open;
	}

	bool WantsMouse() {
	#ifdef OILP_UI_TEXT_EDITOR
		return open || UiTextEditor::IsOpen();
	#else
		return open;
	#endif
	}

	void Toggle() {
		open = !open;
	}

	static bool GlobalIniPath(wchar_t* path) {
		wchar_t appdata[MAX_PATH] = {};
		if (FAILED(SHGetFolderPathW(nullptr, CSIDL_APPDATA, nullptr, 0, appdata)))
			return false;
		wchar_t directory[MAX_PATH] = {};
		wcscpy_s(directory, appdata);
		PathAppendW(directory, L"ShanghaiAlice");
		CreateDirectoryW(directory, nullptr);
		PathAppendW(directory, L"oilp_custom");
		CreateDirectoryW(directory, nullptr);
		wcscpy_s(path, MAX_PATH, directory);
		PathAppendW(path, L"openinputlagpatch.ini");
		return true;
	}

	template <typename Char>
	static std::basic_string<Char> TrimIniText(const std::basic_string<Char>& text) {
		size_t begin = 0;
		while (begin < text.size() && (text[begin] == Char(' ') || text[begin] == Char('\t')))
			++begin;
		size_t end = text.size();
		while (end > begin && (text[end - 1] == Char(' ') || text[end - 1] == Char('\t')))
			--end;
		return text.substr(begin, end - begin);
	}

	template <typename Char>
	static bool NormalizeIniLine(std::basic_string<Char>& line, const std::basic_string<Char>& key) {
		bool carriage_return = !line.empty() && line.back() == Char('\r');
		if (carriage_return)
			line.pop_back();
		const size_t equals = line.find(Char('='));
		if (equals == std::basic_string<Char>::npos)
			return false;
		if (TrimIniText(line.substr(0, equals)) != key)
			return false;

		std::basic_string<Char> normalized = key;
		normalized.push_back(Char(' '));
		normalized.push_back(Char('='));
		normalized.push_back(Char(' '));
		normalized += TrimIniText(line.substr(equals + 1));
		if (carriage_return)
			normalized.push_back(Char('\r'));
		line.swap(normalized);
		return true;
	}

	template <typename Char>
	static bool NormalizeIniText(std::basic_string<Char>& text, const std::basic_string<Char>& key) {
		std::basic_string<Char> normalized;
		normalized.reserve(text.size() + 8);
		bool changed = false;
		size_t start = 0;
		while (start <= text.size()) {
			size_t end = text.find(Char('\n'), start);
			if (end == std::basic_string<Char>::npos)
				end = text.size();
			std::basic_string<Char> line = text.substr(start, end - start);
			if (NormalizeIniLine(line, key))
				changed = true;
			normalized += line;
			if (end < text.size())
				normalized.push_back(Char('\n'));
			else
				break;
			start = end + 1;
		}
		if (changed)
			text.swap(normalized);
		return changed;
	}

	// WritePrivateProfileString writes "key=value" and keeps existing whitespace unpredictably.
	// Rebuild the line as "key = value" so every overlay write has the same shape.
	static void AddIniSpacing(const wchar_t* path, const wchar_t* key) {
		std::ifstream input(path, std::ios::binary);
		if (!input)
			return;
		std::string data((std::istreambuf_iterator<char>(input)),
			std::istreambuf_iterator<char>());
		input.close();
		if (data.size() >= 2 && (unsigned char)data[0] == 0xFF && (unsigned char)data[1] == 0xFE) {
			const size_t count = (data.size() - 2) / sizeof(wchar_t);
			std::wstring text(count, L'\0');
			if (count != 0)
				memcpy(&text[0], data.data() + 2, count * sizeof(wchar_t));
			if (!NormalizeIniText(text, std::wstring(key)))
				return;
			std::ofstream output(path, std::ios::binary | std::ios::trunc);
			output.write("\xFF\xFE", 2);
			output.write((const char*)text.data(), (std::streamsize)(text.size() * sizeof(wchar_t)));
			return;
		}

		char key_ansi[128] = {};
		WideCharToMultiByte(CP_ACP, 0, key, -1, key_ansi, sizeof(key_ansi), nullptr, nullptr);
		if (!NormalizeIniText(data, std::string(key_ansi)))
			return;
		std::ofstream output(path, std::ios::binary | std::ios::trunc);
		output.write(data.data(), (std::streamsize)data.size());
	}

	static void WriteIniValue(const wchar_t* section, const wchar_t* key, const wchar_t* value) {
		wchar_t path[MAX_PATH] = {};
		if (!GlobalIniPath(path))
			return;
		WritePrivateProfileStringW(section, key, value, path);
		AddIniSpacing(path, key);
	}

	// Writes one value back to the global ini, so a change made here survives the restart.
	static void SaveSetting(const wchar_t* section, const wchar_t* key, int value) {
		wchar_t text[32] = {};
		swprintf_s(text, L"%d", value);
		WriteIniValue(section, key, text);
	}

	static void SaveString(const wchar_t* section, const wchar_t* key, const char* value) {
		wchar_t wide[128] = {};
		MultiByteToWideChar(CP_UTF8, 0, value, -1, wide, 128);
		WriteIniValue(section, key, wide);
	}

	// "Ctrl+Shift+O" -> the modifiers and the key it ends with
	struct Hotkey {
		bool ctrl; bool shift; bool alt; int vk;
	};
	static Hotkey ParseHotkey(const char* text) {
		Hotkey key = { true, true, false, 'O' };
		if (text == nullptr || text[0] == '\0')
			return key;
		key.ctrl = strstr(text, "Ctrl") != nullptr || strstr(text, "ctrl") != nullptr;
		key.shift = strstr(text, "Shift") != nullptr || strstr(text, "shift") != nullptr;
		key.alt = strstr(text, "Alt") != nullptr || strstr(text, "alt") != nullptr;
		size_t length = strlen(text);
		if (length > 0) {
			char last = text[length - 1];
			if (last >= 'a' && last <= 'z')
				last = (char)(last - 'a' + 'A');
			if ((last >= 'A' && last <= 'Z') || (last >= '0' && last <= '9'))
				key.vk = last;
		}
		return key;
	}

	// The (?) marker and the restart note, shared by the option helpers
	static void HelpMarker(const char* key) {
		const char* help = OptionDescription(key);
		if (help == nullptr)
			return;
		ImGui::SameLine();
		ImGui::TextDisabled("(?)");
		if (ImGui::IsItemHovered()) {
			// Wrapped so a full sentence still fits on a 480p picture; the explicit \n in a
			// description still starts its own line.
			ImGui::BeginTooltip();
			ImGui::PushTextWrapPos(ImGui::GetFontSize() * 22.0f);
			ImGui::TextUnformatted(help);
			ImGui::PopTextWrapPos();
			ImGui::EndTooltip();
		}
	}

	static void RestartMarker(bool restart) {
		if (!restart)
			return;
		ImGui::SameLine();
		ImGui::TextDisabled("(%s)", S().restart);
	}

	static void OptionBool(const char* label, BOOL* value, const wchar_t* section,
		const wchar_t* key, bool restart) {
		bool checked = *value != FALSE;
		char id[192] = {};
		sprintf_s(id, "%s###%s", OptionDisplayName(label), label);
		if (ImGui::Checkbox(id, &checked)) {
			*value = checked ? TRUE : FALSE;
			SaveSetting(section, key, checked ? 1 : 0);
		}
		RestartMarker(restart);
		HelpMarker(label);
	}

	static void OptionInt(const char* label, int* value, const wchar_t* section,
		const wchar_t* key, int step, int step_fast, bool restart,
		int maximum = 0x7FFFFFFF, int minimum = 0) {
		// These keys are unsigned counts or indexes stored through an int pointer, so the value is
		// held inside its range both before it is drawn and before it goes back to the ini.
		auto clamp = [maximum, minimum](int* v) {
			if (*v < minimum)
				*v = minimum;
			else if (*v > maximum)
				*v = maximum;
		};
		clamp(value);
		ImGui::TextUnformatted(OptionDisplayName(label));
		ImGui::SameLine();
		ImGui::SetNextItemWidth(160.0f);
		char id[192] = {};
		sprintf_s(id, "##%s", label);
		if (ImGui::InputInt(id, value, step, step_fast, ImGuiInputTextFlags_EnterReturnsTrue)) {
			clamp(value);
			SaveSetting(section, key, *value);
		}
		RestartMarker(restart);
		HelpMarker(label);
	}

	// The frame rate settings use the steps thprac does, with the ratio and the resulting frame
	// rate in the slider text, plus a free-drag mode for an exact value.
	static bool free_game_fps = false;
	static bool free_skip_fps = false;
	static bool rate_drafts_ready = false;
	static bool rate_drafts_dirty = false;
	static int draft_game_fps;
	static int draft_skip_fps;
	static int draft_slow_fps;

	static void SyncRateDrafts() {
		if (rate_drafts_dirty)
			return;
		draft_game_fps = Config::GameFPS;
		draft_skip_fps = Config::ReplaySkipFPS;
		draft_slow_fps = Config::ReplaySlowFPS;
		rate_drafts_ready = true;
	}

	static void ApplyRateDrafts() {
		if (!rate_drafts_ready)
			return;
		if (draft_game_fps < 60)
			draft_game_fps = 60;
		if (draft_game_fps > 6000)
			draft_game_fps = 6000;
		if (draft_skip_fps < 60)
			draft_skip_fps = 60;
		if (draft_skip_fps > 6000)
			draft_skip_fps = 6000;
		if (draft_slow_fps < 1)
			draft_slow_fps = 1;
		if (draft_slow_fps > 60)
			draft_slow_fps = 60;

		Config::GameFPS = draft_game_fps;
		Config::ReplaySkipFPS = draft_skip_fps;
		Config::ReplaySlowFPS = draft_slow_fps;
		SaveSetting(L"Option", L"GameFPS", Config::GameFPS);
		SaveSetting(L"Option", L"ReplaySkipFPS", Config::ReplaySkipFPS);
		SaveSetting(L"Option", L"ReplaySlowFPS", Config::ReplaySlowFPS);
		rate_drafts_dirty = false;
	}

	static float RateLabelWidth() {
		const ImGuiStyle& style = ImGui::GetStyle();
		const float game = ImGui::CalcTextSize(OptionDisplayName("GameFPS")).x;
		const float skip = ImGui::CalcTextSize(OptionDisplayName("ReplaySkipFPS")).x;
		const float slow = ImGui::CalcTextSize(OptionDisplayName("ReplaySlowFPS")).x;
		const float widest = game > skip ? (game > slow ? game : slow) : (skip > slow ? skip : slow);
		return widest + style.ItemSpacing.x;
	}

	static void FrameRateSetting(const char* label, int* value,
		int step, int step_count, int max_free, bool infinite_last, bool* free_mode) {
		ImGui::TableNextRow();
		ImGui::TableNextColumn();
		ImGui::AlignTextToFramePadding();
		ImGui::TextUnformatted(OptionDisplayName(label));
		ImGui::TableNextColumn();
		ImGui::SetNextItemWidth(300.0f);
		char id[192] = {};
		sprintf_s(id, "##%s", label);
		if (*free_mode) {
			if (ImGui::DragInt(id, value, 1.0f, 60, max_free))
				rate_drafts_dirty = true;
			if (!ImGui::IsItemActive()) {
				if (*value < 60)
					*value = 60;
				if (*value > max_free)
					*value = max_free;
			}
		} else {
			int index = (*value - 60 + step / 2) / step;
			if (index < 0)
				index = 0;
			if (index > step_count)
				index = step_count;
			char text[64] = {};
			if (infinite_last && index == step_count)
				strcpy_s(text, "infinite");
			else
				sprintf_s(text, "x%.2f (%dfps)", (60.0f + index * step) / 60.0f, 60 + index * step);
			if (ImGui::SliderInt(id, &index, 0, step_count, text)) {
				*value = 60 + index * step;
				rate_drafts_dirty = true;
			}
		}
		ImGui::SameLine();
		char free_id[64] = {};
		sprintf_s(free_id, "##free_%s", label);
		ImGui::Checkbox(free_id, free_mode);
		if (ImGui::IsItemHovered())
			ImGui::SetTooltip("%s", S().free_adjust);
		HelpMarker(label);
	}

	// The replay slow rate is its own range, shown with the ratio against normal speed
	static void SlowRateSetting(int* value) {
		ImGui::TableNextRow();
		ImGui::TableNextColumn();
		ImGui::AlignTextToFramePadding();
		ImGui::TextUnformatted(OptionDisplayName("ReplaySlowFPS"));
		ImGui::TableNextColumn();
		ImGui::SetNextItemWidth(300.0f);
		char text[64] = {};
		sprintf_s(text, "x%.2f (%dfps)", *value / 60.0f, *value);
		if (ImGui::SliderInt("##ReplaySlowFPS", value, 1, 60, text))
			rate_drafts_dirty = true;
		HelpMarker("ReplaySlowFPS");
	}

	static bool OptionChoice(const char* label, int* value, const wchar_t* section, const wchar_t* key,
		const char* const (*choices)[3], int count) {
		ImGui::TextUnformatted(OptionDisplayName(label));
		bool changed = false;
		const int column = Config::Language == 0 ? 1 : Config::Language == 1 ? 0 : 2;
		for (int i = 0; i < count; ++i) {
			if (i != 0)
				ImGui::SameLine();
			char choice_id[160] = {};
			sprintf_s(choice_id, "choice.%s.%d", label, i);
			const char* text = UiText::Get(choice_id);
			if (text == nullptr)
				text = choices[i][column];
			char id[192] = {};
			sprintf_s(id, "%s##%s%d", text, label, i);
			if (ImGui::RadioButton(id, *value == i)) {
				*value = i;
				SaveSetting(section, key, i);
				changed = true;
			}
		}
		HelpMarker(label);
		return changed;
	}

	// Click a key field, then press the combination to use. The modifiers are read at the moment
	// the real key goes down, which is the only time they are all held together.
	static int capturing = 0;   // 0 none, 1 the overlay key, 2 the diagnostic key

	static const char* KeyName(int vk) {
		static char name[16] = {};
		if ((vk >= 'A' && vk <= 'Z') || (vk >= '0' && vk <= '9')) {
			name[0] = (char)vk;
			name[1] = '\0';
			return name;
		}
		if (vk >= VK_F1 && vk <= VK_F12) {
			sprintf_s(name, "F%d", vk - VK_F1 + 1);
			return name;
		}
		switch (vk) {
		case VK_SPACE: return "Space";
		case VK_TAB: return "Tab";
		case VK_RETURN: return "Enter";
		case VK_ESCAPE: return "Esc";
		case VK_INSERT: return "Insert";
		case VK_DELETE: return "Delete";
		case VK_HOME: return "Home";
		case VK_END: return "End";
		case VK_PRIOR: return "PageUp";
		case VK_NEXT: return "PageDown";
		case VK_LEFT: return "Left";
		case VK_RIGHT: return "Right";
		case VK_UP: return "Up";
		case VK_DOWN: return "Down";
		default:
			return nullptr;
		}
	}

	static void PollCapture() {
		if (capturing == 0)
			return;
		static bool was_down[256] = {};
		int pressed = 0;
		for (int vk = 1; vk < 256; ++vk) {
			const bool down = (GetAsyncKeyState(vk) & 0x8000) != 0;
			if (down && !was_down[vk])
				pressed = vk;
			was_down[vk] = down;
		}
		if (pressed == 0)
			return;
		// A bare modifier is not a key on its own, wait for the real one
		if (pressed == VK_CONTROL || pressed == VK_SHIFT || pressed == VK_MENU ||
			pressed == VK_LCONTROL || pressed == VK_RCONTROL || pressed == VK_LSHIFT ||
			pressed == VK_RSHIFT || pressed == VK_LMENU || pressed == VK_RMENU)
			return;
		const char* name = KeyName(pressed);
		if (name == nullptr)
			return;

		char text[64] = {};
		if ((GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0)
			strcat_s(text, "Ctrl+");
		if ((GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0)
			strcat_s(text, "Shift+");
		if ((GetAsyncKeyState(VK_MENU) & 0x8000) != 0)
			strcat_s(text, "Alt+");
		strcat_s(text, name);

		if (capturing == 1) {
			strcpy_s(Config::OverlayKey, text);
			SaveString(L"Imgui", L"OverlayKey", text);
		} else {
			strcpy_s(Config::DiagKey, text);
			SaveString(L"Imgui", L"DiagKey", text);
		}
		capturing = 0;
	}

	static void DrawKeysTab() {
		const ImGuiStyle& style = ImGui::GetStyle();
		const float overlay_label = ImGui::CalcTextSize(S().overlay_key).x;
		const float diag_label = ImGui::CalcTextSize(S().diag_key).x;
		const float label_width = (overlay_label > diag_label ? overlay_label : diag_label) +
			style.ItemSpacing.x;

		if (ImGui::BeginTable("##key_fields", 2, ImGuiTableFlags_SizingFixedFit)) {
			ImGui::TableSetupColumn("##key_labels", ImGuiTableColumnFlags_WidthFixed, label_width);
			ImGui::TableSetupColumn("##key_values", ImGuiTableColumnFlags_WidthStretch);

			ImGui::TableNextRow();
			ImGui::TableNextColumn();
			ImGui::AlignTextToFramePadding();
			ImGui::TextUnformatted(S().overlay_key);
			ImGui::TableNextColumn();
			ImGui::PushID("overlay_key");
			if (ImGui::Button(Config::OverlayKey, ImVec2(240.0f, 0.0f)))
				capturing = capturing == 1 ? 0 : 1;
			ImGui::PopID();
			ImGui::SameLine();
			ImGui::TextDisabled("(?)");
			if (ImGui::IsItemHovered())
				ImGui::SetTooltip("Ctrl+Shift+O");
			if (capturing == 1) {
				ImGui::SameLine();
				ImGui::TextDisabled("%s", S().capture_hint);
			}

			ImGui::TableNextRow();
			ImGui::TableNextColumn();
			ImGui::AlignTextToFramePadding();
			ImGui::TextUnformatted(S().diag_key);
			ImGui::TableNextColumn();
			ImGui::PushID("diag_key");
			if (ImGui::Button(Config::DiagKey, ImVec2(240.0f, 0.0f)))
				capturing = capturing == 2 ? 0 : 2;
			ImGui::PopID();
			if (capturing == 2) {
				ImGui::SameLine();
				ImGui::TextDisabled("%s", S().capture_hint);
			}
			ImGui::EndTable();
		}

		ImGui::Separator();
		ImGui::TextUnformatted(S().language);
		int language = Config::Language;
		if (ImGui::RadioButton(UiText::LanguageLabel(0), language == 0)) { SetLanguage(0); SaveSetting(L"Imgui", L"Language", 0); }
		ImGui::SameLine();
		if (ImGui::RadioButton(UiText::LanguageLabel(1), language == 1)) { SetLanguage(1); SaveSetting(L"Imgui", L"Language", 1); }
		ImGui::SameLine();
		if (ImGui::RadioButton(UiText::LanguageLabel(2), language == 2)) { SetLanguage(2); SaveSetting(L"Imgui", L"Language", 2); }

#ifdef OILP_UI_TEXT_EDITOR
		if (UiTextEditor::Enabled()) {
			ImGui::Separator();
			ImGui::TextDisabled("Developer");
			if (ImGui::Button("Text editor"))
				UiTextEditor::Toggle();
			ImGui::SameLine();
			ImGui::TextDisabled("Ctrl+Shift+E");
		}
#endif
	}

	static void DrawInfoTab() {
		ImGui::Text("%s: %.2f", S().info_logic, Limiter::game_fps);
		ImGui::Text("%s: %.2f", S().info_present, Limiter::PresentRate());
		ImGui::Text("%s: %.1f fps", S().info_shown, overlay_present_rate());
	}

	// Every key the ini has, in the order the file lists them. Labels come from kOptionName, the
	// (?) text from kOptionHelp.
	static void DrawConfigTab() {
		ImGui::TextDisabled("[Imgui]");
		OptionBool("UseInGameImguiOverlay", &Config::UseInGameImguiOverlay, L"Imgui", L"UseInGameImguiOverlay", true);
		OptionBool("ShowPatchAppliedMessage", &Config::ShowPatchAppliedMessage, L"Imgui", L"ShowPatchAppliedMessage", false);

		ImGui::TextDisabled("[Rendering]");
		OptionBool("FixBulletPixelSnapping", &Config::FixBulletPixelSnapping, L"Rendering", L"FixBulletPixelSnapping", false);
		{
			int form = 3;
			if (Config::InterpolationFPS > 0)
				form = 4;
			else if (Config::Interpolation == 0)
				form = 0;
			else if (Config::Interpolation == 1)
				form = 1;
			else if (Config::Interpolation == -1)
				form = 2;

			ImGui::TextUnformatted(OptionDisplayName("Interpolation"));
			const int column = Config::Language == 0 ? 1 : Config::Language == 1 ? 0 : 2;
			for (int i = 0; i < 5; ++i) {
				if (i != 0)
					ImGui::SameLine();
				char choice_id[160] = {};
				sprintf_s(choice_id, "choice.Interpolation.%d", i);
				const char* text = UiText::Get(choice_id);
				if (text == nullptr)
					text = kInterpolationForms[i][column];
				char id[160] = {};
				sprintf_s(id, "%s##interpolation%d", text, i);
				if (ImGui::RadioButton(id, form == i)) {
					form = i;
					switch (i) {
					case 0:
						Config::Interpolation = 0;
						Config::InterpolationFPS = 0;
						break;
					case 1:
						Config::Interpolation = 1;
						Config::InterpolationFPS = 0;
						break;
					case 2:
						Config::Interpolation = -1;
						Config::InterpolationFPS = 0;
						break;
					case 3:
						if (Config::Interpolation < 2)
							Config::Interpolation = 2;
						Config::InterpolationFPS = 0;
						break;
					default:
						if (Config::InterpolationFPS == 0)
							Config::InterpolationFPS = 144;
						Config::Interpolation = 0;
						break;
					}
					char text[32] = {};
					if (Config::InterpolationFPS > 0)
						sprintf_s(text, "*%u", Config::InterpolationFPS);
					else
						sprintf_s(text, "%d", Config::Interpolation);
					SaveString(L"Rendering", L"Interpolation", text);
				}
			}
			HelpMarker("Interpolation");
			if (form == 3) {
				ImGui::SetNextItemWidth(160.0f);
				if (ImGui::InputInt("##per_frame", &Config::Interpolation, 1, 10, ImGuiInputTextFlags_EnterReturnsTrue)) {
					if (Config::Interpolation < 2)
						Config::Interpolation = 2;
					char text[32] = {};
					sprintf_s(text, "%d", Config::Interpolation);
					SaveString(L"Rendering", L"Interpolation", text);
				}
			} else if (form == 4) {
				int rate = (int)Config::InterpolationFPS;
				ImGui::SetNextItemWidth(160.0f);
				if (ImGui::InputInt("##fixed_rate", &rate, 10, 60, ImGuiInputTextFlags_EnterReturnsTrue)) {
					if (rate < 1)
						rate = 1;
					Config::InterpolationFPS = (UINT)rate;
					char text[32] = {};
					sprintf_s(text, "*%u", Config::InterpolationFPS);
					SaveString(L"Rendering", L"Interpolation", text);
				}
			}
		}
		OptionBool("PlayerBody", &Config::InterpolatePlayerBody, L"Rendering", L"PlayerBody", false);
		OptionBool("PlayerShots", &Config::InterpolatePlayerShots, L"Rendering", L"PlayerShots", false);
		OptionBool("BulletsAndLasers", &Config::InterpolateBulletsAndLasers, L"Rendering", L"BulletsAndLasers", false);

		ImGui::TextDisabled("[Option]");
		SyncRateDrafts();
		const float rate_label_width = RateLabelWidth();
		if (ImGui::BeginTable("##game_rate", 2, ImGuiTableFlags_SizingFixedFit)) {
			ImGui::TableSetupColumn("##game_rate_labels", ImGuiTableColumnFlags_WidthFixed, rate_label_width);
			ImGui::TableSetupColumn("##game_rate_values", ImGuiTableColumnFlags_WidthStretch);
			FrameRateSetting("GameFPS", &draft_game_fps, 15, 8, 6000, false, &free_game_fps);
			ImGui::EndTable();
		}
		OptionBool("ReplaySpeedControl", &Config::ReplaySpeedControl, L"Option", L"ReplaySpeedControl", false);
		if (ImGui::BeginTable("##replay_rates", 2, ImGuiTableFlags_SizingFixedFit)) {
			ImGui::TableSetupColumn("##replay_rate_labels", ImGuiTableColumnFlags_WidthFixed, rate_label_width);
			ImGui::TableSetupColumn("##replay_rate_values", ImGuiTableColumnFlags_WidthStretch);
			FrameRateSetting("ReplaySkipFPS", &draft_skip_fps, 60, 20, 6000, true, &free_skip_fps);
			SlowRateSetting(&draft_slow_fps);
			if (rate_drafts_dirty) {
				ImGui::TableNextRow();
				ImGui::TableNextColumn();
				ImGui::TableNextColumn();
				if (ImGui::Button(T("button.apply")))
					ApplyRateDrafts();
			}
			ImGui::EndTable();
		}
		OptionInt("BltPrepareTime", (int*)&Config::BltPrepareTime, L"Option", L"BltPrepareTime", 1, 10, false, 16);
		{
			int sleep = (int)Config::Sleep;
			if (OptionChoice("Sleep", &sleep, L"Option", L"Sleep", kSleepChoices, 2))
				Config::Sleep = (SleepType)sleep;
		}
		OptionBool("D3D9Ex", &Config::D3D9Ex, L"Option", L"D3D9Ex", true);
		{
			int rate = (int)Config::FullscreenRefreshRate;
			if (OptionChoice("FullscreenRefreshRate", &rate, L"Option", L"FullscreenRefreshRate", kRefreshChoices, 3))
				Config::FullscreenRefreshRate = (TargetRefreshRate)rate;
		}
		OptionBool("ForceRatio_4_3", &Config::ForceRatio43, L"Option", L"ForceRatio_4_3", false);
		{
			int readout = Config::ShowOverlay;
			if (OptionChoice("ShowOverlay", &readout, L"Option", L"ShowOverlay", kOverlayChoices, 4))
				Config::ShowOverlay = readout;
		}
		OptionBool("DebugConsole", &Config::DebugConsole, L"Option", L"DebugConsole", true);
		OptionBool("DebugWait", &Config::DebugWait, L"Option", L"DebugWait", true);
		OptionBool("FixTH07Cherry&BonusDisplay", &Config::FixTH07CherryBonusDisplay, L"Option", L"FixTH07Cherry&BonusDisplay", true);
		OptionBool("FixTH10ReplayResave", &Config::FixTH10ReplayResave, L"Option", L"FixTH10ReplayResave", true);
		OptionBool("FixTH12ScoreDisplay", &Config::FixTH12ScoreDisplay, L"Option", L"FixTH12ScoreDisplay", true);
		OptionBool("FixPracModeReplayCrash", &Config::FixPracModeReplayCrash, L"Option", L"FixPracModeReplayCrash", true);
		OptionBool("FixInputGlitching", &Config::FixInputGlitching, L"Option", L"FixInputGlitching", false);
		OptionBool("CalcFPS", &Config::CalcFPS, L"Option", L"CalcFPS", false);
		OptionBool("AlwaysBlt", &Config::AlwaysBlt, L"Option", L"AlwaysBlt", false);
		OptionInt("GameOverride", (int*)&Config::GameOverride, L"Option", L"GameOverride", 1, 1, true,
			0x7FFFFFFF, -1);

		ImGui::TextDisabled("[Window]");
		OptionBool("AskWindowMode", &Config::AskWindowMode, L"Window", L"AskWindowMode", true);
		OptionBool("enabled", &Config::WindowEnabled, L"Window", L"enabled", true);
		OptionInt("X", (int*)&Config::WindowX, L"Window", L"X", 1, 100, true);
		OptionInt("Y", (int*)&Config::WindowY, L"Window", L"Y", 1, 100, true);
		OptionInt("Width", (int*)&Config::WindowWidth, L"Window", L"Width", 1, 100, true);
		OptionInt("Height", (int*)&Config::WindowHeight, L"Window", L"Height", 1, 100, true);
		OptionBool("TitleBar", &Config::WindowTitleBar, L"Window", L"TitleBar", true);
		OptionBool("AlwaysOnTop", &Config::WindowAlwaysOnTop, L"Window", L"AlwaysOnTop", true);
		{
			int cover = Config::WindowCoverTaskbar;
			if (OptionChoice("CoverTaskbar", &cover, L"Window", L"CoverTaskbar", kTaskbarChoices, 3))
				Config::WindowCoverTaskbar = cover;
		}

		ImGui::TextDisabled("[Experimental]");
		OptionBool("BypassNvidiaD3D8Detour", &Config::BypassNvidiaD3D8Detour, L"Experimental", L"BypassNvidiaD3D8Detour", true);
	}

	// The cursor is released while the panels are open so the pointer can reach them; closing them
	// hands the pointer back.
	static bool mouse_free = false;
	static void SetMouseFree(bool free) {
		if (free == mouse_free)
			return;
		mouse_free = free;
		ImGuiIO& io = ImGui::GetIO();
		io.MouseDrawCursor = free;
		if (free) {
			ClipCursor(nullptr);
		} else {
			RECT rect = {};
			if (GetClientRect(GetActiveWindow(), &rect)) {
				POINT center = { (rect.right - rect.left) / 2, (rect.bottom - rect.top) / 2 };
				ClientToScreen(GetActiveWindow(), &center);
				SetCursorPos(center.x, center.y);
			}
		}
	}

	bool Install(void* hwnd) {
		if (installed)
			return true;
		if (!Enabled() || !hwnd)
			return false;
		installed = true;

		RegisterTexts();
#ifdef OILP_UI_TEXT_EDITOR
		UiText::Load();
		UiTextEditor::SetWindow(hwnd);
#else
		UiText::ApplyBaked();
#endif
		IMGUI_CHECKVERSION();
		ImGui::CreateContext();
		ImGuiIO& io = ImGui::GetIO();
		io.IniFilename = nullptr;
		io.LogFilename = nullptr;
		// The device's canvas is not readable yet, and the font size depends on it: the window
		// client area is a good stand-in for the picture size at this point.
		RECT client = {};
		if (GetClientRect((HWND)hwnd, &client) && client.right > client.left && client.bottom > client.top)
			io.DisplaySize = ImVec2((float)(client.right - client.left), (float)(client.bottom - client.top));
		io.FontDefault = LoadLanguageFont(Config::Language);
		Gui::ImplWin32Init(hwnd);
		Gui::ImplWin32HookWndProc();

		if (Config::ShowPatchAppliedMessage)
			hint_until = GetTickCount() + 3000;
		Trace("Install done\n");
		return true;
	}

	void PollHotkey() {
		if (!installed)
			return;
		static Hotkey key = ParseHotkey(Config::OverlayKey);
		static char cached[64] = {};
		if (strcmp(cached, Config::OverlayKey) != 0) {
			strcpy_s(cached, Config::OverlayKey);
			key = ParseHotkey(Config::OverlayKey);
		}
		const bool down = (!key.ctrl || (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0)
			&& (!key.shift || (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0)
			&& (!key.alt || (GetAsyncKeyState(VK_MENU) & 0x8000) != 0)
			&& (GetAsyncKeyState(key.vk) & 0x8000) != 0;
		if (down && !hotkey_down) {
			open = !open;
		}
		hotkey_down = down;
	}

	// The panels are laid out for a 720p picture; larger windows get scaled up so the text and the
	// controls keep the same physical size on screen. 480p gets 80% of that.
	static ImGuiStyle base_style;
	static bool base_style_saved = false;

	void NewFrame() {
		if (!installed)
			return;
		static unsigned int calls = 0;
		++calls;
		if (calls <= 10)
			Trace("NewFrame %u: display=%.0fx%.0f\n", calls, ImGui::GetIO().DisplaySize.x,
				ImGui::GetIO().DisplaySize.y);
		// A resize changes the size the glyphs have to be rasterised at, so the atlas gets rebuilt
		const ImFont* current = ImGui::GetIO().FontDefault;
		if (current != nullptr) {
			const float delta = current->FontSize - FontRasterSize();
			if (delta > 0.5f || delta < -0.5f)
				pending_language = Config::Language;
		}
		ApplyPendingLanguage();
		PollCapture();
		PollHotkey();
#ifdef OILP_UI_TEXT_EDITOR
		UiTextEditor::PollHotkey();
		const bool editor_open = UiTextEditor::IsOpen();
#else
		const bool editor_open = false;
#endif
		static unsigned int font_revision = UiText::FontRevision();
		if (font_revision != UiText::FontRevision()) {
			font_revision = UiText::FontRevision();
			pending_language = Config::Language;
		}
		const bool menu_active = open || editor_open;
		SetMouseFree(menu_active);
		if (!base_style_saved) {
			base_style = ImGui::GetStyle();
			base_style_saved = true;
		}
		const float scale = DisplayScale();
		ImGuiStyle style = base_style;
		style.ScaleAllSizes(scale);
		// ScaleAllSizes rounds this one down, and at 0.8 that lands on zero, which would hide the
		// software cursor entirely on a 480p screen
		style.MouseCursorScale = scale;
		ImGui::GetStyle() = style;
		Gui::ImplWin32NewFrame(menu_active);
		ImGui::NewFrame();
	}

	void Render() {
		if (!installed)
			return;

		if (hint_until) {
			if (GetTickCount() > hint_until) {
				hint_until = 0;
			} else {
				ImGuiIO& io = ImGui::GetIO();
				ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x - 12, io.DisplaySize.y - 12),
					ImGuiCond_Always, ImVec2(1.0f, 1.0f));
				ImGui::SetNextWindowBgAlpha(0.55f);
				ImGui::Begin("##oilp_hint", nullptr,
					ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_AlwaysAutoResize);
				ImGui::TextUnformatted(S().hint);
				ImGui::End();
			}
		}

		if (open) {
			const float scale = DisplayScale();
			const ImVec2 display = ImGui::GetIO().DisplaySize;
			// Centred and kept inside the picture. ImGui's default position is a fixed offset,
			// which puts most of a 620-wide panel outside a 640x480 screen.
			float width = 620.0f * scale;
			float height = 460.0f * scale;
			if (display.x > 0.0f && width > display.x * 0.92f)
				width = display.x * 0.92f;
			if (display.y > 0.0f && height > display.y * 0.92f)
				height = display.y * 0.92f;
			ImGui::SetNextWindowSize(ImVec2(width, height), ImGuiCond_FirstUseEver);
			ImGui::SetNextWindowPos(ImVec2(display.x * 0.5f, display.y * 0.5f),
				ImGuiCond_FirstUseEver, ImVec2(0.5f, 0.5f));
			if (ImGui::Begin("OpenInputLagPatch", &open)) {
				if (ImGui::BeginTabBar("##tabs")) {
					if (ImGui::BeginTabItem(S().keys)) {
						DrawKeysTab();
						ImGui::EndTabItem();
					}
					if (ImGui::BeginTabItem(S().info)) {
						DrawInfoTab();
						ImGui::EndTabItem();
					}
					if (ImGui::BeginTabItem(S().config)) {
						DrawConfigTab();
						ImGui::EndTabItem();
					}
					ImGui::EndTabBar();
				}
			}
			ImGui::End();
		}

#ifdef OILP_UI_TEXT_EDITOR
		UiTextEditor::Draw();
#endif
		ImGui::Render();
	}
}
