简体中文 | [English](README.en.md)

# OpenInputLagPatch

一个用于替代[Vpatch](https://ux.getuploader.com/swmplv75e/) 的补丁，目前仍处于**高度开发中**，用于修复东方系列作品的限帧与输入延迟问题。

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

OpenInputLagPatch 通过游戏可执行文件同目录下的 `openinputlagpatch.ini` 进行配置。没有该文件游戏也能正常运行，随时可以手动创建一份。下面是一份示例配置（内容与随包提供的那份一致，注释为英文）：

```
[Option]

; Target framerate of the game
; Going above 60 doesn't make the game any smoother, it just makes it faster
; Default: 60
GameFPS = 60

; Frame interpolation (motion smoothing): draws extra frames between the game's own ones, so the
; game stays at its own frame rate but is presented more often
; 0: off, -1: pick the largest multiple of the game's frame rate the display can show,
; N: present N times per game frame, *R: present at R frames per second (e.g. *144)
; Only implemented for Touhou 15, and only its bullets are moved forward so far
; Default: 0
Interpolation = 0

; Allow controlling the speed of replay playback
; This will override the in-game replay speed control if it exists
; Default: 1
ReplaySpeedControl = 1

; Target framerate for replay skipping (hold LCTRL or attack key during replay playback)
; Default: 240
ReplaySkipFPS = 240

; Target framerate for replay slowdown (hold LSHIFT during replay playback)
; Default: 30
ReplaySlowFPS = 30

; Amount of time in milliseconds before screen refresh to try running the game logic
; Lower is better, but it comes with a higher risk of the game not being able to run in time and causing more input lag
; Default: 2
BltPrepareTime = 2

; Chooses the method used for waiting between frames
; 0: Basic spinwait (very accurate, but uses lots of CPU), 1: vpatch (slightly higher risk of skipping a frame depending on PC, but uses way less CPU)
; Default: 1
Sleep = 1

; Use Direct3D9Ex instead of Direct3D9 to allow reducing input lag by 0 to 2 frames
; Has no effect on Touhou 9.5 and below if d3d8to9 isn't being used
; (dgVoodoo2 doesn't go through D3D9 either, but it presents with a flip model swapchain,
;  which is at least as good for input lag)
; Default: 1
D3D9Ex = 1

; Chooses the refresh rate used by the game
; 0: Maximum refresh rate, 1: 60hz, 2: Largest multiple of 60 (60hz, 120hz, 180hz, 240hz, etc.)
; Default: 2
FullscreenRefreshRate = 2

; Draws a small overlay in the bottom left of the screen showing how long it takes the game to render
; Useful for tweaking BltPrepareTime
; Default: 1
ShowOverlay = 1

; Opens a console for debugging purposes
; Default: 0
DebugConsole = 0

; Opens a message box on boot to allow attaching a debugger
; Default: 0
DebugWait = 0

; Fixes the input glitching issues that can occur in EoSD and PCB if you deselect the window while the game is loading for example
; Default: 0
FixInputGlitching = 0

; Keeps rendering while the game window isn't active, so the picture doesn't freeze after
; switching away from it (same idea as vpatch's AlwaysBlt)
; Only implemented for th06 and th07
; Default: 1
AlwaysBlt = 1

; Overrides game detection to a specific game
; See games.h for possible values
; CHANGING THIS WILL PROBABLY BREAK EVERYTHING. Only use this if you are 100% sure that the game's offsets are exactly the same!
; Default: -1
GameOverride = -1

[Window]

; Asks whether the game should run in fullscreen mode on boot, so the game's own
; fullscreen setting doesn't have to be changed.
; Answering no makes the game run in window mode, answering yes makes it run in fullscreen
; Only Touhou 6 to 9.5 are asked, because the later games have their own option for this
; Default: 0
AskWindowMode = 0

; Whether the settings below are applied to the game's window
; They only take effect while the game runs in window mode
; Default: 0
enabled = 0

; Position of the top left corner of the window, in pixels
; 2147483648 (0x80000000) lets Windows pick the position
; Default: 2147483648
X = 2147483648
Y = 2147483648

; Size of the window, in pixels
; 0 keeps the size the game would normally use
; Default: 640
Width = 640

; Default: 480
Height = 480

; Whether the window keeps its title bar
; Default: 1
TitleBar = 1

; Whether the window is always on top of other windows
; This can cover the taskbar while enabled
; Default: 0
AlwaysOnTop = 0
```

# 技术细节

*（仅红魔乡与妖妖梦）* 游戏主循环被改为在游戏更新逻辑**之后**再执行绘制逻辑（与原本的顺序相反），可减少约 1 帧输入延迟。

`Direct3DCreate9` 被钩住并改用 `Direct3DCreate9Ex`，从而可以使用 `IDirect3DDevice9Ex::SetMaximumFrameLatency`，再减少 0~2 帧输入延迟。**这个优化只对老的 blt 模型呈现有效**（原生 D3D9，或者用 d3d8to9 的场合）：装了 dgVoodoo2 之后游戏是通过 DXGI 的 flip 模型交换链呈现的，队列本身已经压到最短，这个优化无事可做——dgVoodoo2 的 flip 模型也正是它在实测中延迟最低的原因。

补帧（`Interpolation`）把每一游戏帧呈现多次：游戏仍然按自己的帧率跑逻辑，补丁在游戏自己那次呈现之后、下一帧逻辑开始之前，把同一份状态再画几遍并呈现（60fps 的游戏在 180hz 上就是 0、半帧、一帧各呈现一次）。zun 的引擎把逻辑与绘制分开，所以重画同一份状态本身是安全的；要让额外帧里的画面确实在动，补丁在每次额外呈现之前把子弹按「速度 × 这一帧的几分之几」前移（和游戏自己的 `位置 += 速度` 同一套算法），画完立刻还原，游戏自己的逻辑永远看不到这些改动。额外呈现走的是设备自己的 `Present`，不经过 thprac 等工具替换掉的那个（它们按"一游戏帧一次呈现"来测输入延迟，多吃到额外呈现会让读数变成整帧的几分之一）；工具在这一帧已经建好的覆盖层会被重画进每次额外呈现，但不会重跑它的 UI 与输入代码（按住方向键不会在它菜单里连跳）。目前只有绀珠传（th15）实现了补帧，而且只有子弹会被前移。

最后，游戏自带的限帧器被禁用，改用大幅简化的 vpatch 限帧器。

# TODO

- 支持更多作品
- 修 bug、加新功能
- 把补帧扩展到其它作品，并补上绀珠传的敌人与直线激光
- 看看 vpatch 的 `AutoBltPrepareTime` 算法（不确定）
- 可能还有我忘了的
