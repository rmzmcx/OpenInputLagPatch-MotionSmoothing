# Changelog

English | [简体中文](CHANGELOG.zh-CN.md)

Changes made on top of upstream [OpenInputLagPatch](https://github.com/khang06/OpenInputLagPatch).

Only changes that have been tested in-game and confirmed working are listed here, so this file
is also the record of what has actually been verified rather than just what was attempted.

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
