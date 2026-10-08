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
