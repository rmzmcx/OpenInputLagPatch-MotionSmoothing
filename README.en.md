[简体中文](README.md) | English

# OpenInputLagPatch

A **heavily work-in-progress** replacement for [vpatch](https://ux.getuploader.com/swmplv75e/), which fixes frame limiter and input lag issues in Touhou games.

# Usage
1. Compile as x86/Release
2. Copy openinputlagpatch.dll and oilp_loader.exe to your game directory
3. *(Touhou 9.5 and below)* Install [dgVoodoo2](https://github.com/dege-diosg/dgVoodoo2) (recommended: its flip model presentation has the lowest input lag) or [d3d8to9](https://github.com/crosire/d3d8to9) **(The ENB converter will not work!)**
4. Run oilp_loader.exe or use the latest thprac (tick the option "Use OpenInputLagPatch(if available)") in one specific game selection page

# Compatibility
Not every game is supported, but support for more games is actively being worked on.

| Game                                 | Supported | Replay speed control |
|--------------------------------------|-----------|----------------------|
| Embodiment of Scarlet Devil          |✅|✅|
| Perfect Cherry Blossom               |✅|✅|
| Imperishable Night                   |✅|✅|
| Phantasmagoria of Flower View        |✅|✅|
| Shoot the Bullet                     |✅|✅|
| Mountain of Faith                    |✅|✅|
| Uwabami Breakers                     |❌|❔|
| Subterranean Animism                 |✅|✅|
| Undefined Fantastic Object           |✅|✅|
| Double Spoiler                       |✅|✅|
| Great Fairy Wars                     |✅|✅|
| Ten Desires                          |✅|✅|
| Double Dealing Character             |✅|✅|
| Impossible Spell Card                |✅|✅|
| Legacy of Lunatic Kingdom            |✅|✅|
| Hidden Star in Four Seasons          |✅|✅|
| Violet Detector                      |✅|✅|
| Wily Beast and Weakest Creature      |✅|✅|
| Unconnected Marketeers               |✅|✅|
| 100th Black Market                   |✅|N/A|
| Unfinished Dream of All Living Ghost |✅|N/A|
| Fossilized Wonders                   |✅|✅|
| Danmakufu v0.12m                     |❌|❔|
| Danmakufu ph3                        |❌|❔|

# Configuration
OpenInputLagPatch is configured via a file called `openinputlagpatch.ini` stored in the same directory as the game executable. The game will run perfectly fine without the config file, but there's always the option of manually creating one. Here's an example config file:
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
; 目前对东方辉针城（th14）与东方绀珠传（th15）生效：子弹、自机本体（含子机与自机弹）、
; 直线激光会被外推；敌人本体不做，曲线激光不做。开启 ShowOverlay 时，会在左下角「渲染耗时」
; 那行字的右边显示实际的呈现帧率。
;
; 取值：0 关闭；-1 取显示屏刷新率里游戏帧率的最大整数倍；N 每游戏帧呈现 N 次；
;       *R 按固定呈现率（例如 *144，小数部分会在帧与帧之间累计）。
;
; Frame interpolation (motion smoothing): draws extra frames between the game's own ones, so the
; game stays at its own frame rate but is presented more often.
;
; The extra frames are made by *extrapolating*: every object that is moved forward is put where it
; will be a fraction of a frame from now (its position plus its velocity times that fraction),
; drawn, and put back again, so the game's own logic and its random numbers never see it. No
; previous frame is kept and nothing is interpolated backwards - this is forward extrapolation.
;
; Touhou 14 and 15 for now: their bullets, the player itself (with its options and its shots) and
; the straight lasers are extrapolated; the enemies aren't interpolated and the curve lasers are
; left alone. With ShowOverlay on, the rate the frames are actually presented at is shown next to
; the render time line in the bottom left corner.
;
; 0: off, -1: pick the largest multiple of the game's frame rate the display can show,
; N: present N times per game frame, *R: present at R frames per second (e.g. *144)
;
; 默认 / Default: -1
Interpolation = -1

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
; 默认 / Default: 0
Sleep = 0

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

; 在画面左下角画一个小 overlay，显示游戏渲染这一帧花了多久，用来调 BltPrepareTime。
; 补帧开启时，游戏区右边还会显示实际的呈现帧率（每秒呈现多少次）。
;
; Draws a small overlay in the bottom left of the screen showing how long it takes the game to
; render; with the interpolation on, the rate the frames are actually presented at is shown at the
; right edge of the play area as well. Useful for tweaking BltPrepareTime.
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

; 修掉红魔乡/妖妖梦里取消窗口焦点（比如加载时点别处）会出现的输入卡顿问题。
;
; Fixes the input glitching issues that can occur in EoSD and PCB if you deselect the window while
; the game is loading for example.
;
; 默认 / Default: 0
FixInputGlitching = 0

; 游戏窗口失去焦点时继续渲染并呈现，切出去画面不会冻住（相当于 vpatch 的 AlwaysBlt）。
; 只对 th06 与 th07 实现。
;
; Keeps rendering while the game window isn't active, so the picture doesn't freeze after switching
; away from it (same idea as vpatch's AlwaysBlt). Only implemented for th06 and th07.
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

# Technical details
*(Touhou 6-7 only)* The game loop is modified to run the drawing logic *after* the game update logic instead of the other way around, which should shave off a frame of input lag.

`Direct3DCreate9` is hooked to use `Direct3DCreate9Ex` instead, which allows the use of `IDirect3DDevice9Ex::SetMaximumFrameLatency`, which should shave off an additional 0 to 2 frames of input lag. **This only does anything for the old blt model presentation** (native D3D9, or D3D8 through d3d8to9): with dgVoodoo2 the game is presented through a DXGI flip model swapchain, whose queue is already as short as it gets, so there is nothing left for this to save - that flip model is also why dgVoodoo2 measures best for input lag.

Frame interpolation (`Interpolation`) presents every game frame more than once: the game keeps running at its own frame rate, and the patch draws the same state again right after the game's own present, spreading the extra presentations over the rest of the frame (a 60fps game on a 180hz display gets one at 0, half a frame and one frame). ZUN's engine separates the logic from the rendering, so drawing the same state again is safe by itself; for the extra presentations to show motion, the patch moves the bullets forward by "velocity × the fraction of a frame" before each of them - the same movement the game applies itself - and puts them back right after the render, so the game's own logic never sees the moved positions. The extras go through the device's own Present rather than the hooks tools put in front of it (which measure the input latency assuming one present per game frame, and would report a fraction of a frame with the extras), and the overlay a tool has already built is drawn into every one of them without running any of its UI or input code (holding a direction key won't skip through its menus). Only Touhou 15 has this so far, and only the bullets are moved forward.

Finally, the in-game frame limiter is disabled and a heavily simplified version of vpatch's frame limiter is used.

# TODO
- Support more games
- fix bugs and add more features
- extend the frame interpolation to more games, and to the enemies and the straight lasers of Touhou 15
- Take a look at vpatch's `AutoBltPrepareTime` algorithm(Not sure)
- Probably more stuff I forgot
