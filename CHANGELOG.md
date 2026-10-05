# Changelog

English | [简体中文](CHANGELOG.zh-CN.md)

Changes made on top of upstream [OpenInputLagPatch](https://github.com/khang06/OpenInputLagPatch).

Only changes that have been tested in-game and confirmed working are listed here, so this file
is also the record of what has actually been verified rather than just what was attempted.

## 2026-10-06

### Added

- **The overlay works for D3D8 games that never go through D3D9** (`d0cc56a`, `ed756d7`)

  The overlay was D3D9 only - it is created in the D3D9 CreateDevice hook and drawn in the
  EndScene hook - so it worked with d3d8to9 and DXVK, which translate D3D8 to D3D9, but not
  with dgVoodoo2, which translates D3D8 straight to D3D11, or with no wrapper at all. A D3D8
  version of the overlay now draws with the D3D8 device the game already uses, and it is only
  used while there is no D3D9 device, so the wrappers that do go through D3D9 keep using the
  D3D9 overlay.

- **`FullscreenRefreshRate` is applied to the D3D8 games too** (`5d1d73f`)

  The option only existed in the D3D9 hook, so it did nothing for the D3D8 titles: they build
  their presentation parameters through IDirect3D8, and since the games ask for the current
  desktop rate, every value looked like it forced the maximum refresh rate. The D3D8
  CreateDevice and Reset hooks choose the rate the same way now, and let the runtime pick
  instead of failing when the display has no suitable mode.

- **vsync is forced off for the D3D8 games too** (`1dd1bb2`)

  Setting `D3DPRESENT_INTERVAL_IMMEDIATE` (together with `D3DSWAPEFFECT_DISCARD` and a back
  buffer count of 0) only happened in the D3D9 hook, so with dgVoodoo2 or without a wrapper
  the games kept presenting with the interval they asked for and replay skipping stayed capped
  by the display refresh rate. The D3D8 hook does the same now, for fullscreen devices - D3D8
  requires its two FullScreen_* fields to stay zero while windowed.

### Fixed

- **Alt-tabbing back into an exclusive fullscreen D3D8 game quit the game** (`d0cc56a`)

  The games release their `D3DPOOL_DEFAULT` surfaces and then call `Reset`, and they quit when
  that fails. D3D8's Reset fails while any default pool resources are still alive, and the
  overlay's own resources were released after the reset instead of before it, so a fullscreen
  D3D8 game exited as soon as the window was focused again.

- **The overlay moved to the corner of the pause screen on D3D8 games** (`ed756d7`)

  State blocks don't carry the viewport around, so the overlay was drawn with whatever
  viewport the game had set - and PCB's pause screen draws the picture into a smaller area. It
  sets its own viewport and transforms on every draw now, the same way thprac's overlays do.

### Changed

- **The missing D3D8 wrapper notice is a console message, and it recommends dgVoodoo2** (`d4296fc`, `5df484c`)

  A D3D8 game that never goes through D3D9 only loses the D3D9Ex input lag reduction now that
  the overlay and the other D3D8 settings work without it. That is worth a line in the debug
  console instead of a message box on every launch, and the recommendation is dgVoodoo2 first,
  since its flip model presentation measures best for input lag.

- **The reference config ships with default values** (`dee2a1e`)

  `openinputlagpatch.ini` had the values from testing (BltPrepareTime 0, Sleep 0, the window
  mode settings and so on) instead of the defaults its own comments document, which is
  confusing when comparing the two. Every entry is at its default now, so the file documents
  the options without turning anything on by itself, and the README recommends dgVoodoo2 for
  the D3D8 games as well.

## 2026-10-05

### Added

- **Window mode: the same `[Window]` configuration as vpatch, and a question about the screen
  mode on boot** (`26b310a`)

  `openinputlagpatch.ini` now has vpatch's `[Window]` section: `AskWindowMode`, `enabled`,
  `X`, `Y`, `Width`, `Height`, `TitleBar` and `AlwaysOnTop`, with the same meaning as in
  `vpatch.ini`. While a game runs in window mode its window is moved and resized to those
  values, and the title bar and always on top options are applied.

  With `AskWindowMode` turned on, Touhou 6 to 9.5 ask whether they should start in fullscreen
  before they create their window, so the screen mode no longer has to be changed in the
  games' own settings. The answer is written into the game's own "windowed" flag, the same way
  vpatch does it, instead of only forcing the D3D presentation parameters: with dgVoodoo2 set
  up to emulate fullscreen as a borderless window (`AppControlledScreenMode = false`,
  `FullscreenAttributes = fake`) that request is ignored, so a game whose own setting says
  windowed kept behaving like a windowed one.

  The question is asked right after the game creates its single instance mutex, which is the
  earliest point where tools like thprac can still detect the game, and changing the mode from
  inside the game afterwards keeps working. Only Touhou 6 to 9.5 are asked, because the later
  games have their own option for it.

## 2026-10-04

### Fixed

- **Spurious "couldn't hook Direct3DCreate9" warning when using dgVoodoo2** (`d20a521`)

  dgVoodoo2 translates D3D8 to D3D11, so its `d3d8.dll` has no `Direct3DCreate9` import to hook,
  and the patch showed the warning even though the game runs fine. The loaded `d3d8.dll` is now
  identified precisely by its version resource (`ProductName` contains `dgVoodoo`, checked across
  all language/codepage blocks) and the warning is skipped only in that case. An unknown wrapper
  still gets the warning, and d3d8to9 still takes the normal hook path.

- **Touhou 6 with dgVoodoo2: acceleration could not exceed the display refresh rate** (`667fe5e`)

  dgVoodoo2 turns the game's D3D8 present into a D3D11 flip-model swapchain, and that present
  blocks on vblank once the queue is full. Since the game logic presents once per frame, the game
  speed was pinned to the display refresh rate, so replay skip and thprac speedup could not go any
  faster. The D3D8 present is now hooked (vtable index 15) and submitted every Nth frame, with
  `N = ceil(target framerate / refresh rate)`, and only while the target framerate is above the
  refresh rate. At normal framerates `N` is 1, so every present is forwarded unchanged.

  Currently installed for Touhou 6 only, since the cap appears to be specific to that game -
  the later D3D8 titles are unaffected and the D3D9 titles use a different present path.

- **New `AlwaysBlt` option: keep rendering while the game window isn't active** (`12fe031`, `c41c53d`, `ea884df`)

  Switching away from the window froze the picture. Two things caused it: the games stop
  rendering on their own once they lose focus (`GameWindow::Render` returns immediately while
  the window isn't active), and oilp's window update hook stopped calling the game's window
  update while unfocused - which is also what drives rendering, so nothing was drawn at all.

  AlwaysBlt now bypasses the early return (turning the conditional jump at `0x004206F0` for
  th06 and `0x004346F0` for th07 into an unconditional one) and lets the window update keep
  running while unfocused, the same behaviour as vpatch's AlwaysBlt option. It is enabled by
  default and can be turned off with the `AlwaysBlt` entry in `openinputlagpatch.ini`; a
  reference copy of that config now ships alongside the build.

  Implemented for th06 and th07 only.
