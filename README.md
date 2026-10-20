简体中文 | [English](README.en.md) | [日本語](README.ja.md)

# OpenInputLagPatch

一个用于替代[Vpatch](https://ux.getuploader.com/swmplv75e/) 的补丁，目前仍处于**高度开发中**，用于修复东方系列作品的限帧与输入延迟问题。
**注意：该fork的新增功能大量依赖于Vibe Coding，因此可能有更多代码史山和BUG，欢迎在issue提出改进意见和反馈BUG**

联系我们
[![Discord][discord-badge]][discord] 点击加入zmcx的Discord群组

QQ群群号：1128317778

# 使用

1. 以 x86/Release 编译
2. 把 `openinputlagpatch.dll` 和 `oilp_loader.exe` 复制到游戏目录
3. *（th09.5 及以下）* 安装 [dgVoodoo2](https://github.com/dege-diosg/dgVoodoo2)（推荐，flip 模型呈现延迟最低）或者 [d3d8to9](https://github.com/crosire/d3d8to9)——**（ENB converter 无法生效！）**
4. 运行 `oilp_loader.exe`，或使用最新版 thprac（在对应作品的启动页面勾选 "使用OpenInputLagPatch（如果可用）"）

# 兼容性

并非所有作品都受支持，支持范围仍在持续扩充中。

| 作品 | 支持 | Replay 速度控制 |
|---------------------------------|-----------|----------------------|
| 东方红魔乡 |✅|✅|
| 东方妖妖梦 |✅|✅|
| 东方永夜抄 |✅|✅|
| 东方花映塚 |✅|✅|
| 东方文花帖 |✅|✅|
| 东方风神录 |✅|✅|
| 黄昏酒场 |❌|❔|
| 东方地灵殿 |✅|✅|
| 东方星莲船 |✅|✅|
| 东方文花帖DS |✅|✅|
| 妖精大战争 |✅|✅|
| 东方神灵庙 |✅|✅|
| 东方辉针城 |✅|✅|
| 弹幕天邪鬼 |✅|✅|
| 东方绀珠传 |✅|✅|
| 东方天空璋 |✅|✅|
| 秘封噩梦日记 |✅|✅|
| 东方鬼形兽 |✅|✅|
| 东方虹龙洞 |✅|✅|
| 弹幕狂们的黑市 |✅|N/A|
| 东方兽王园 |✅|N/A|
| 东方锦上京 |✅|✅|
| 弹幕风 v0.12m |❌|❔|
| 弹幕风 ph3 |❌|❔|

# 配置

OpenInputLagPatch 通过 ini 配置。补丁启动时会先读取并应用全局配置 `%APPDATA%\ShanghaiAlice\oilp_custom\openinputlagpatch.ini`；如果这份全局配置不存在，会自动创建一份带默认值的。随后再读取游戏可执行文件同目录下的 `openinputlagpatch.ini`，如果存在，其中出现的键会覆盖全局配置；没有该文件时只使用全局配置。下面是一份示例配置（内容与随包提供的那份一致，注释为中英双语）：

```
; OpenInputLagPatch 参考配置（[Interpolation]、[Option] 与 [Window] 三节），注释为中英双语，键名与取值保持不变。
;
; OpenInputLagPatch reference config ([Interpolation], [Option] and [Window]), with bilingual
; comments. The keys and their values are unchanged.
;
; 全局配置默认在 %APPDATA%\ShanghaiAlice\oilp_custom\openinputlagpatch.ini，不存在时会自动创建；
; 游戏目录下的同名文件会覆盖其中出现的键。
;
; The global config defaults to %APPDATA%\ShanghaiAlice\oilp_custom\openinputlagpatch.ini and is
; created automatically when it does not exist; a file with the same name in the game directory
; overrides the keys it contains.

[Interpolation]

; 补帧（动态平滑）：在游戏自己的帧之间多画几帧，游戏保持自己的帧率，但呈现得更频繁。
;
; 多出来的那些帧是「外推」出来的：把每个要动的物体按「位置 += 速度 × 这一帧的小数部分」前移，
; 画出来之后再立刻还原，所以游戏自己的逻辑和随机数完全不受影响。这里没有保存上一帧、也没有做
; 反向插值——是单向的向前外推（extrapolation），不是常见的双向插值。
;
; 目前对东方红魔乡（th06）、东方妖妖梦（th07）、东方永夜抄（th08）、东方风神录（th10）、东方地灵殿（th11）、东方星莲船（th12）、东方神灵庙（th13）、东方辉针城（th14）、东方绀珠传（th15）、
; 东方天空璋（th16）、东方鬼形兽（th17）、东方虹龙洞（th18）、东方兽王园（th19）与东方锦上京（th20）生效。
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

; 上面的 Interpolation 是总开关。以下几个开关只在总开关开启时，选择要外推的内容。
; The Interpolation setting above is the master switch. The following switches select what is
; extrapolated while the master switch is on.
;
; 是否补帧自机本体（包括子机/option、orb 等跟随自机的东西）。
; Whether to extrapolate the player body (including options, orbs and other things that follow it).
;
; 是否补帧自机子弹。
; Whether to extrapolate the player's own shots.
;
; 是否补帧敌弹和直线激光。
; Whether to extrapolate enemy bullets and straight lasers.
;
; 默认 / Default: 1
PlayerBody = 1
PlayerShots = 1
BulletsAndLasers = 1

[Option]

; 目标帧率。
; 超过 60 不会让画面更顺滑，只会让游戏跑得更快。
;
; Target framerate of the game.
; Going above 60 doesn't make the game any smoother, it just makes it faster.
;
; 默认 / Default: 60
GameFPS = 60

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

# 技术细节

*（仅红魔乡与妖妖梦）* 游戏主循环被改为在游戏更新逻辑**之后**再执行绘制逻辑（与原本的顺序相反），可减少约 1 帧输入延迟。

`Direct3DCreate9` 被钩住并改用 `Direct3DCreate9Ex`，从而可以使用 `IDirect3DDevice9Ex::SetMaximumFrameLatency`，再减少 0~2 帧输入延迟。**这个优化只对老的 blt 模型呈现有效**（原生 D3D9，或者用 d3d8to9 的场合）：装了 dgVoodoo2 之后游戏是通过 DXGI 的 flip 模型交换链呈现的，队列本身已经压到最短，这个优化无事可做——dgVoodoo2 的 flip 模型也正是它在实测中延迟最低的原因。

补帧（`Interpolation`）把每一游戏帧呈现多次：游戏仍然按自己的帧率跑逻辑，补丁在游戏自己那次呈现之后、下一帧逻辑开始之前，把同一份状态再画几遍并呈现（60fps 的游戏在 180hz 上就是 0、半帧、一帧各呈现一次）。zun 的引擎把逻辑与绘制分开，所以重画同一份状态本身是安全的；要让额外帧里的画面确实在动，补丁在每次额外呈现之前把要动的物体按「速度 × 这一帧的几分之几」前移（和游戏自己的 `位置 += 速度` 同一套算法），画完立刻还原，游戏自己的逻辑永远看不到这些改动。额外呈现走的是设备自己的 `Present`，不经过 thprac 等工具替换掉的那个（它们按"一游戏帧一次呈现"来测输入延迟，多吃到额外呈现会让读数变成整帧的几分之一）；工具在这一帧已经建好的覆盖层会被重画进每次额外呈现，但不会重跑它的 UI 与输入代码（按住方向键不会在它菜单里连跳）。目前红魔乡（th06）、妖妖梦（th07）、永夜抄（th08）与风神录（th10）、地灵殿（th11）、星莲船（th12）、神灵庙（th13）、辉针城（th14）、绀珠传（th15）、天空璋（th16）、鬼形兽（th17）、虹龙洞（th18）、兽王园（th19）与锦上京（th20）实现了补帧：子弹、自机本体（连同子机与自机弹，鬼形兽里还包括咆哮/hyper 的特效与特殊自机弹；虹龙洞的自机弹系统与 th17 不同，另外还包括能力卡在自机身上画的特效精灵，以及商店买卡界面的文字修复；兽王园是对战作品，两侧各有一套管理器，所以上面每一项都是两侧一起做的，它的自机弹也不在子弹管理器的那几条链上，和直线激光一样是按每帧量出来的位移外推的；锦上京跑的是同一套引擎但它是单人作，自机弹是自机自己池子里的 256 格（位置在格内 +0x50、精灵 id 在 +0x18），而且它的绘制位是更新期烘焙进精灵的，所以格内位置与精灵 +0x5BC 两处都写；它的文字管理器不在全局里，是在构造处抓下来的；永夜抄是更早那一代的引擎，自机/自机弹/子机的绘制位都是绘制期才从逻辑位写进精灵的，所以自机在"画它的那一句"上做、自机弹与子机改的是各自的源位置；子机的四个槽用的是运行时才挂上去的绘制回调，五队共用同一个绘制函数；自机弹、子机与直线激光的运动由脚本/跟随决定、没有速度字段可读，都是按每帧量出来的位移外推的；红魔乡与它同代，有三处这个引擎特有的地方：它的自机弹画的是**精灵自己的位置**（逻辑位另有一份，额外帧只动精灵那份、逻辑位一个字节都不碰），它的自机带**两颗 orb**（火力 8 以上才出现，就是这一作的"子机"——由聚焦状态机摆位，自机弹也从它出发），而额外帧必须把这一趟开画时的**深度缓冲**一起还原（游戏的计算链每帧清一次 Z；不还原的话这一趟先画的东西会被上一趟后画的 HUD/本体用深度挡住，表现为魔法阵与背景局部闪烁），**摄像机也要还两份**（全局的 `viewMatrix`/`projectionMatrix` 是舞台裁剪与投影用的，设备上另有一份）；它的 AnmManager 还把贴图检查嵌在"精灵变了"那个分支里，所以额外帧开画前要作废**整套** current* 缓存（只作废贴图会画出没有贴图的白色方块））、直线激光都会在额外帧里前移，菜单/暂停这类界面的文字也不会被额外帧多画一遍（永夜抄的字符串表本来就是"画完即清"，天然不会重复）。敌人本体不做——它们的画面位置由动画脚本决定，没有能读出来的速度，与其按猜的做不如明确排除；曲线激光同样不做。

最后，游戏自带的限帧器被禁用，改用大幅简化的 vpatch 限帧器。

# 已知问题

- 高倍率补帧长时间游玩后可能出现补帧下滑，详见 [KNOWN_ISSUES.md](KNOWN_ISSUES.md)。

# TODO

- 支持更多作品
- 修 bug、加新功能
- 把补帧扩展到其它作品；敌人本体与曲线激光目前按需求不做
- 看看 vpatch 的 `AutoBltPrepareTime` 算法（不确定）
- 可能还有我忘了的

[discord]: https://discord.gg/baE2vffeMj
[discord-badge]: https://img.shields.io/discord/1174662479385735188?color=%237289DA&logo=discord&logoColor=%23FFFFFF
