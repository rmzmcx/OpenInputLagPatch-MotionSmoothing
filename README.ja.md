[简体中文](README.md) | [English](README.en.md) | 日本語

# OpenInputLagPatch

[vpatch](https://ux.getuploader.com/swmplv75e/) の代わりとなるパッチで、東方 Project の各作品のフレームリミッターと入力遅延の問題を修正します。まだ**開発途中**です。
**注意：この fork で追加された機能の多くは Vibe Coding（AI を使ったコーディング）に大きく頼っているため、通常よりも出来の悪いコードやバグが多い可能性があります。改善案やバグの報告は GitHub の Issue にお願いします。**

お問い合わせ
[![Discord][discord-badge]][discord] クリックすると zmcx の Discord サーバーに参加できます。

QQ グループ番号：1128317778

# 使い方

1. x86/Release でビルドします（または Releases からビルド済みのものをダウンロードします）
2. `openinputlagpatch.dll` と `oilp_loader.exe` をゲームのフォルダにコピーします
3. *（th09.5 以前）* [dgVoodoo2](https://github.com/dege-diosg/dgVoodoo2)（推奨。flip モデルでの表示のため入力遅延が最も少ない）または [d3d8to9](https://github.com/crosire/d3d8to9) を導入します——**（ENB converter では動作しません！）**
4. `oilp_loader.exe` を実行します。または最新版の thprac を使い、各作品の起動画面で「OpenInputLagPatch を使用（利用可能な場合）」にチェックを入れます

# 対応状況

すべての作品に対応しているわけではなく、対応作品は順次増やしています。

| 作品 | 対応 | リプレイ速度の調整 |
|---------------------------------|-----------|----------------------|
| 東方紅魔郷 |✅|✅|
| 東方妖々夢 |✅|✅|
| 東方永夜抄 |✅|✅|
| 東方花映塚 |✅|✅|
| 東方文花帖 |✅|✅|
| 東方風神録 |✅|✅|
| 黄昏酒場 |❌|❔|
| 東方地霊殿 |✅|✅|
| 東方星蓮船 |✅|✅|
| ダブルスポイラー |✅|✅|
| 妖精大戦争 |✅|✅|
| 東方神霊廟 |✅|✅|
| 東方輝針城 |✅|✅|
| 弾幕アマノジャク |✅|✅|
| 東方紺珠伝 |✅|✅|
| 東方天空璋 |✅|✅|
| 秘封ナイトメアダイアリー |✅|✅|
| 東方鬼形獣 |✅|✅|
| 東方虹龍洞 |✅|✅|
| バレットフィリア達の闇市場 |✅|N/A|
| 東方獣王園（v1.10c / v1.00a） |✅|N/A|
| 東方錦上京 |✅|✅|
| 弾幕風 v0.12m |❌|❔|
| 弾幕風 ph3 |❌|❔|

# 設定

ゲームの exe と同じフォルダにある `openinputlagpatch.ini` で設定します。このファイルが無くてもゲームは普通に動き、必要ならいつでも自分で作れます。以下は設定例です（同梱の ini と同じ内容で、コメントは中国語と英語です）：

```
; OpenInputLagPatch 参考配置（[Option] 与 [Window] 两节），注释为中英双语，键名与取值保持不变。
;
; OpenInputLagPatch reference config ([Option] and [Window]), with bilingual comments. The keys
; and their values are unchanged.

[Option]

; 目标帧率。
; 超过 60 不会让画面更顺滑，只会让游戏跑得更快。
;
; Target framerate of the game.
; Going above 60 doesn't make the game any smoother, it just makes it faster.
;
; 默认 / Default: 60
GameFPS = 60

; 补帧（动态平滑）：在游戏自己的帧之间多画几帧，游戏保持自己的帧率，但呈现得更频繁。
;
; 多出来的那些帧是「外推」出来的：把每个要动的物体按「位置 += 速度 × 这一帧的小数部分」前移，
; 画出来之后再立刻还原，所以游戏自己的逻辑和随机数完全不受影响。这里没有保存上一帧、也没有做
; 反向插值——是单向的向前外推（extrapolation），不是常见的双向插值。
;
; 目前对东方红魔乡（th06）、东方妖妖梦（th07）、东方永夜抄（th08）、东方风神录（th10）、东方地灵殿（th11）、东方星莲船（th12）、东方神灵庙（th13）、东方辉针城（th14）、东方绀珠传（th15）、
; 东方天空璋（th16）、东方鬼形兽（th17）、东方虹龙洞（th18）、东方兽王园（th19）与东方锦上京（th20）生效：
; 子弹、自机本体（含子机/option 与自机弹）、直线激光会被外推；敌人本体不做，曲线激光不做。
; ShowOverlay 勾了「帧率」这一项时，会显示实际呈现率（每秒呈现多少次）。
;
; 取值：0 关闭；1 补到显示屏刷新率（144hz 显示器就呈现 144fps，60fps 逻辑则是每帧 2/3 次交替）；-1 取显示屏刷新率里游戏帧率的最大整数倍（间隔最均匀）；
;       N 每游戏帧呈现 N 次；*R 按固定呈现率（例如 *144，小数部分会在帧与帧之间累计）。
;
; Frame interpolation (motion smoothing): draws extra frames between the game's own ones, so the
; game stays at its own frame rate but is presented more often.
;
; The extra frames are made by *extrapolating*: every object that is moved forward is put where it
; will be a fraction of a frame from now (its position plus its velocity times that fraction),
; drawn, and put back again, so the game's own logic and its random numbers never see it. No
; previous frame is kept and nothing is interpolated backwards - this is forward extrapolation.
;
; Touhou 6, 7, 8, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19 and 20 for now: their bullets, the
; player itself (with its options or orbs and its shots) and the straight lasers are extrapolated;
; the enemies aren't interpolated and the curve lasers are left alone. When ShowOverlay asks for the
; rate readout, the rate the frames are actually presented at is shown as well.
;
; 0: off, 1: present at the display's own refresh rate (a 144hz display shows 144fps, which for
; 60fps logic is 2 and 3 presentations a frame alternating), -1: pick the largest whole multiple of
; the game's frame rate the display can show (evenly spaced presentations),
; N: present N times per game frame, *R: present at R frames per second (e.g. *144)
;
; 检测到 Ju.N.Owen（th19 的非官方联机工具，游戏目录下有 modules\th19_junowen.dll）时，补帧会被强制关闭。
; Ju.N.Owen（th19 の非公式対戦ツール。ゲームのフォルダに modules\th19_junowen.dll がある場合）を検出すると、補間は強制的に無効になります。
; When Ju.N.Owen (th19's unofficial netplay tool, modules\th19_junowen.dll in the game's directory) is
; installed, the interpolation is always turned off.
;
; 默认 / Default: 1
Interpolation = 1

; 是否允许控制 replay 回放速度（会覆盖游戏自带的回放速度控制，如果有的话）。
;
; Allow controlling the speed of replay playback.
; This will override the in-game replay speed control if it exists.
;
; 默认 / Default: 1
ReplaySpeedControl = 1

; 回放快进时的目标帧率（回放中按住 LCTRL 或射击键）。
;
; Target framerate for replay skipping (hold LCTRL or attack key during replay playback).
;
; 默认 / Default: 240
ReplaySkipFPS = 240

; 回放慢放时的目标帧率（回放中按住 LSHIFT）。
;
; Target framerate for replay slowdown (hold LSHIFT during replay playback).
;
; 默认 / Default: 30
ReplaySlowFPS = 30

; 距屏幕刷新多少毫秒时开始跑游戏逻辑。越小越好，但太小会让游戏来不及跑完，反而增加输入延迟。
;
; Amount of time in milliseconds before screen refresh to try running the game logic.
; Lower is better, but it comes with a higher risk of the game not being able to run in time and
; causing more input lag.
;
; 默认 / Default: 2
BltPrepareTime = 2

; 帧间等待方式：0 纯自旋等待（非常准，但很吃 CPU）；1 vpatch 的做法（视机器可能有跳帧风险，但 CPU 占用低得多）。
;
; Chooses the method used for waiting between frames.
; 0: Basic spinwait (very accurate, but uses lots of CPU), 1: vpatch (slightly higher risk of
; skipping a frame depending on PC, but uses way less CPU).
;
; 默认 / Default: 1
Sleep = 1

; 用 Direct3D9Ex 取代 Direct3D9，可以再减少 0~2 帧输入延迟。
; th09.5 及以下如果没有用 d3d8to9 则无效。
; （dgVoodoo2 也不走 D3D9，但它用 flip 模型交换链呈现，对输入延迟至少同样好。）
;
; Use Direct3D9Ex instead of Direct3D9 to allow reducing input lag by 0 to 2 frames.
; Has no effect on Touhou 9.5 and below if d3d8to9 isn't being used.
; (dgVoodoo2 doesn't go through D3D9 either, but it presents with a flip model swapchain, which is
; at least as good for input lag.)
;
; 默认 / Default: 1
D3D9Ex = 1

; 游戏使用的刷新率：
; 0 最高刷新率；1 固定 60hz；2 60 的最大整数倍（60/120/180/240hz 等）。
;
; Chooses the refresh rate used by the game.
; 0: Maximum refresh rate, 1: 60hz, 2: Largest multiple of 60 (60hz, 120hz, 180hz, 240hz, etc.).
;
; 默认 / Default: 2
FullscreenRefreshRate = 2

; 画补丁自己的小 overlay，两行读数可以分别开关（它们的和就是这个值）：
;   0  都不显示（等于完全没有 overlay）
;   1  「帧率」：实际呈现率（每秒呈现多少次）。只有补帧开着时才有意义，所以补帧关闭时这一项不显示
;      （单独显示时它就贴在左下角）
;   2  「延迟」：左下角那行渲染耗时（`0.65/2.00ms` = 游戏自己这一帧花了多久 / BltPrepareTime 的设定值，调 BltPrepareTime 就看它）
;   3  两个都显示
;
; Draws the patch's own little overlay; its two readouts are switched on separately (this value is
; their sum):
;   0  neither (no overlay at all)
;   1  the rate readout: how many frames a second are actually presented. It only means something
;      while the interpolation is on, so it isn't shown with the interpolation off (on its own it
;      sits at the left edge)
;   2  the render time line: "0.65/2.00ms", how long the game's own part of the frame took and what
;      BltPrepareTime is set to (the line that setting is tuned with)
;   3  both
;
; 默认 / Default: 1
ShowOverlay = 1

; 打开控制台窗口，方便调试。
;
; Opens a console for debugging purposes.
;
; 默认 / Default: 0
DebugConsole = 0

; 启动时弹一个消息框，方便挂调试器。
;
; Opens a message box on boot to allow attaching a debugger.
;
; 默认 / Default: 0
DebugWait = 0

; 修掉红魔乡/妖妖梦里「窗口不在前台时 DirectInput 拒绝给状态，游戏却只把『设备丢失』当失败，
; 于是继续读一份从没被填过的缓冲区」造成的乱按键、按键卡住（相当于 vpatch 的 FixInputGlitching；
; vpatch 是在它的 AlwaysBlt 里一起做的）。
;
; 永夜抄与妖妖梦那种「自机读的是上一帧输入副本」的 1 帧输入延迟**不在这个选项里**：那两处是
; vpatch 无条件生效的代码，oilp 现在也一样无条件生效，与本选项无关。
;
; Fixes the keyboard state EoSD and PCB read while their window is not in the foreground: the
; device refuses to hand the state over, but the game only treats "device lost" as a failure and
; goes on reading a buffer that was never filled in - keys show up as pressed out of nowhere, and
; they can stay stuck (vpatch does this as part of its AlwaysBlt).
;
; The input-timing fix for PCB and IN - the player reading a copy of the input that the game only
; advances later - is NOT this option: vpatch applies it unconditionally, and so does oilp now.
;
; 默认 / Default: 1
FixInputGlitching = 1

; 是否接管游戏自己测量的帧率（相当于 vpatch 的 CalcFPS，默认同为 1）。
;
; 红魔乡 / 妖妖梦 / 永夜抄 每一帧给一个计数器 +1，每半秒用这段时间除一次，得到自己的帧率；
; 这个读数决定它把那一秒里多少算作"丢掉的帧率"（结果画面显示的处理落ち率，以及录像里记的
; 那个值，都是从这里算出来的）。只要有一个半秒窗口落在读盘 / 切换场景的卡顿上，这一局的
; 处理落ち率就会被那一次拖住（所以短关卡、过场待得久都会看到几个百分点）。
;
; 打开后本补丁不再让游戏自己算：改用限帧器实测的帧率交给它（和 vpatch 的 CalcFPS=1 同一个
; 思路），处理落ち率因此保持 0.00%、帧率文字也不再忽高忽低。关掉则完全按原版行为。
;
; Whether the patch should replace the frame rate the game measures for itself (vpatch's CalcFPS,
; which also defaults to 1).
;
; EoSD / PCB / IN add one to a counter every frame, divide it by the elapsed half second, and use
; that reading to decide how much of the half second they lost: it is where the processing rate on
; the results screen and the value a replay records come from. One window landing on a load or a
; scene change is enough to drag a whole run's reading with it, which is why short stages and time
; spent between stages show up as a few percent.
;
; On, the patch measures the frame rate itself and hands the game that instead (the same idea as
; vpatch's CalcFPS=1), so the processing rate stays 0.00% and the frame rate text stops wandering.
; Off leaves the game to measure for itself.
;
; 默认 / Default: 1
CalcFPS = 1

; 游戏窗口失去焦点时继续渲染并呈现，切出去画面不会冻住（相当于 vpatch 的 AlwaysBlt）。
; 只对 th06 与 th07 实现。
;
; Keeps rendering while the game window isn't active, so the picture doesn't freeze after switching
; away from it (same idea as vpatch's AlwaysBlt). Only implemented for th06 and th07.
;
; 红魔乡/妖妖梦里失焦时整个 Render 会直接返回，所以只有这一项打开、FixInputGlitching 关掉，
; 才会真的跑进上面那个「读未初始化缓冲区」的坑里。
;
; In EoSD and PCB the whole render bails out while unfocused, so it takes this option with
; FixInputGlitching off to actually run into the uninitialised-buffer bug described above.
;
; 默认 / Default: 1
AlwaysBlt = 1

; 覆盖游戏检测结果，强制按指定作品处理。
; 取值见 games.h。
; 改这个大概率会直接弄坏一切，只有在 100% 确定该作品偏移完全一样时才用！
;
; Overrides game detection to a specific game.
; See games.h for possible values.
; CHANGING THIS WILL PROBABLY BREAK EVERYTHING. Only use this if you are 100% sure that the game's
; offsets are exactly the same!
;
; 默认 / Default: -1
GameOverride = -1

[Window]

; 启动时询问这次是否用全屏运行，这样不必去改游戏自己的全屏设置。
; 答「否」用窗口模式，答「是」用全屏。
; 只有东方 6~9.5 会被询问，之后的几作自带这个选项。
;
; Asks whether the game should run in fullscreen mode on boot, so the game's own fullscreen
; setting doesn't have to be changed. Answering no makes the game run in window mode, answering yes
; makes it run in fullscreen. Only Touhou 6 to 9.5 are asked, because the later games have their
; own option for this.
;
; 默认 / Default: 0
AskWindowMode = 0

; 是否套用下面这些窗口设置。它们只在游戏处于窗口模式时生效。
;
; Whether the settings below are applied to the game's window.
; They only take effect while the game runs in window mode.
;
; 默认 / Default: 0
enabled = 0

; 窗口左上角坐标（像素）。2147483648（0x80000000）表示让 Windows 自己决定位置。
;
; Position of the top left corner of the window, in pixels.
; 2147483648 (0x80000000) lets Windows pick the position.
;
; 默认 / Default: 2147483648
X = 2147483648
Y = 2147483648

; 窗口大小（像素）。0 表示保持游戏原本的大小。
;
; Size of the window, in pixels. 0 keeps the size the game would normally use.
;
; 默认 / Default: 640
Width = 640

; 默认 / Default: 480
Height = 480

; 窗口是否保留标题栏。
;
; Whether the window keeps its title bar.
;
; 默认 / Default: 1
TitleBar = 1

; 窗口是否始终保持在其他窗口之上（开启时会盖住任务栏）。
;
; Whether the window is always on top of other windows.
; This can cover the taskbar while enabled.
;
; 默认 / Default: 0
AlwaysOnTop = 0

; 是否在游戏窗口处于前台时让任务栏「让开」，而不像 AlwaysOnTop 那样一直压在其它窗口之上。
; 0 关闭；1 告诉 shell 这个窗口是全屏窗口，让任务栏退开（窗口本身仍是普通窗口）；2 只在处于前台时把窗口放进置顶层。
; 两种做法在窗口不是前台时都不会有任何效果（非置顶窗口本来就位于任务栏之下）；切到别的窗口后，游戏会正常让位。
;
; Whether the taskbar is kept out of the way while the game's window is the active window, without
; holding the window above every other one the way AlwaysOnTop does.
; 0: off, 1: tell the shell the window is fullscreen so it steps the taskbar aside,
; 2: hold the window on top only while it is the active window.
; The window is a normal one either way: switch to something else and it covers the game again.
;
; 默认 / Default: 0
CoverTaskbar = 0
```

## GameOverride

`GameOverride` は `[Option]` の項目です。既定値の `-1` では実行ファイルのハッシュから作品を
自動判定します。値を指定するとハッシュ検査を省略し、選択した作品のアドレス表を強制的に使います。
実行ファイルの版、イメージベース、関数配置が対応ビルドと完全に一致する場合だけ使用してください。
誤った値は関係ないアドレスへの書き込みやゲームのクラッシュにつながります。

| ゲーム | GameOverride の値 |
|---|---:|
| 自動判定（推奨） | `-1` |
| `custom.exe`。パッチ導入処理が直ちに終了するため、実際には何も適用されません | `0` |
| TH06 v1.02h | `1` |
| TH07 v1.00b | `3` |
| TH08 v1.00d | `4` |
| TH09 v1.50a | `5` |
| TH09.5 v1.02a | `14` |
| TH10 v1.00a | `2` |
| TH11 v1.00a | `6` |
| TH12 v1.00b | `7` |
| TH12.5 v1.00a | `15` |
| TH12.8 v1.00a | `16` |
| TH13 v1.00c | `8` |
| TH14 v1.00b | `9` |
| TH14.3 v1.00a | `17` |
| TH15 v1.00b | `10` |
| TH16 v1.00a | `11` |
| TH16.5 v1.00a | `18` |
| TH17 v1.00b | `12` |
| TH18 v1.00a | `13` |
| TH18.5 v1.00a | `19` |
| TH19 v1.10c | `20` |
| TH19 v1.00a | `22` |
| TH20 v1.00c | `21` |

たとえば、改造された東方文花帖の実行ファイルにパッチを強制する場合：

```ini
[Option]
GameOverride = 14
```

# 技術的な詳細

*（紅魔郷と妖々夢のみ）* ゲームのメインループを、描画処理をゲームの更新処理の**後**に実行するように変更しています（本来とは逆の順番）。これで入力遅延が約1フレーム減ります。

`Direct3DCreate9` をフックして `Direct3DCreate9Ex` を使うようにしており、`IDirect3DDevice9Ex::SetMaximumFrameLatency` が使えるため、さらに 0〜2 フレーム入力遅延が減ります。**この最適化が効くのは古い blt モデルの表示だけです**（素の D3D9、または d3d8to9 を使う場合）。dgVoodoo2 を入れるとゲームは DXGI の flip モデルのスワップチェーンで表示され、キューはすでに最短なので、この最適化でできることはありません。dgVoodoo2 の flip モデルが実測で最も遅延が少ないのもこのためです。

フレーム補間（`Interpolation`）は、1つのゲームフレームを複数回表示します。ゲームのロジックは本来のフレームレートのまま動き、パッチはゲーム自身の表示の直後から次のフレームのロジックが始まるまでの間に、同じ状態をもう何回か描いて表示します（60fps のゲームを 180hz で表示すると、0・半フレーム・1フレームの位置でそれぞれ1回ずつ表示します）。ZUN のエンジンはロジックと描画が分かれているので、同じ状態を描き直すこと自体は安全です。追加のフレームで画面がちゃんと動いて見えるように、パッチは追加の表示の前に、動かす物体を「速度 × そのフレーム内での経過割合」だけ前に進め（ゲーム自身の `位置 += 速度` と同じ計算）、描き終わったらすぐ元に戻します。ゲーム自身のロジックがこの変更を見ることはありません。追加の表示はデバイス自身の `Present` を通り、thprac などのツールが差し替えた Present は通りません（それらは「1ゲームフレームに1回の表示」を前提に入力遅延を測るため、追加の表示を含めると読みがフレームの何分の一かになってしまいます）。ツールがそのフレームですでに作ったオーバーレイは追加の表示のたびに描き直されますが、ツールの UI や入力処理が再実行されることはありません（方向キーを押しっぱなしにしてもメニューが連続で進むことはありません）。

現在フレーム補間に対応しているのは、紅魔郷（th06）・妖々夢（th07）・永夜抄（th08）と、風神録（th10）・地霊殿（th11）・星蓮船（th12）・神霊廟（th13）・輝針城（th14）・紺珠伝（th15）・天空璋（th16）・鬼形獣（th17）・虹龍洞（th18）・獣王園（th19、v1.10c のみ）・錦上京（th20）です。弾、自機本体（オプションや自機の弾を含む）、直線レーザーが追加のフレームで前に進み、メニューやポーズ画面の文字が追加のフレームで二重に描かれることもありません。作品ごとの細かな違い（自機の弾の持ち方、カメラや深度バッファの扱いなど）は [README.md](README.md) と [README.en.md](README.en.md) の同じ節に詳しく書かれています。敵本体は対象外です。敵の画面上の位置はアニメーションのスクリプトで決まり、読み取れる速度が無いためで、推測で動かすよりも明確に除外しています。曲線レーザーも対象外です。

最後に、ゲーム内蔵のフレームリミッターは無効にし、大幅に簡略化した vpatch のフレームリミッターを使っています。

# 既知の問題

- ステージの切り替わりで、背景のフェードイン・フェードアウトが本来より速く見えることがあります。ゲーム自体は本来の速さで動いています。

# TODO

- 対応作品を増やす
- バグを直し、機能を追加する
- フレーム補間を他の作品にも広げる（敵本体と曲線レーザーは今のところ要件どおり対象外）
- vpatch の `AutoBltPrepareTime` のアルゴリズムを調べる（未定）
- たぶん忘れていることがまだある

[discord]: https://discord.gg/baE2vffeMj
[discord-badge]: https://img.shields.io/discord/1174662479385735188?color=%237289DA&logo=discord&logoColor=%23FFFFFF
