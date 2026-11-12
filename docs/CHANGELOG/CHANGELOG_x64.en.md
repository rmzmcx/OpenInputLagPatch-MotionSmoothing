# x64 Changelog

[简体中文](CHANGELOG_x64.zh-CN.md) | English | [日本語](CHANGELOG_x64.ja.md)

This file records features and changes on the standalone x64 branch. It is maintained separately
from the changelogs for the x86 games.

## 2026-11-12

### Added

- **The x64 runtime for Touhou 6 NC now supports object-level frame interpolation** (`37a6ad9`)

  The x64 runtime for `th06nc.exe` now preserves the original Update Chain rate and only repeats
  the game's Render Chain, final composition, and Present. Extra frames reuse the game's native
  D3D11 rendering path and existing Present/overlay hook, while object and rendering state is
  restored around each pass. No Update Chain runs during extra Render or Compose.

  The current coverage includes ordinary enemy bullets in state 1, the player and both options,
  player shots, and straight lasers. Enemy bullets use their current velocity and state scale;
  the player uses the current input and character speed table; options, player shots, and lasers
  use measured motion between logic frames. Reuse guards check object type, logical position, and
  draw position. Laser angles use the shortest arc.

  Enemy bullets and lasers do not advance in extra frames while time is stopped, the game is
  paused, or object updates are blocked. The game's native FPS accounting is saved and restored
  around the extra final composition, so its FPS text continues to report the real logic/main-loop
  rate. Test logs show about 60fps of main-loop updates and zero Update calls during extra Render
  or Compose.

  `[Interpolation]` accepts the x86 formats `0`, `1`, `-1`, `N`, and `*R`, with separate
  `EnemyBullets`, `Lasers`, `PlayerBody`, and `PlayerShots` switches. The D3D11 swap chain can
  optionally use `ALLOW_TEARING`, so submissions above the display refresh rate can affect latency
  or tearing. Official thprac UI replay state is discovered through a `thprac64.dll` version table;
  UI replay is disabled for an unrecognised version.

  Enemy bodies and curved lasers are not covered yet. The exact pre-spawn empty-slot hook for
  player shots is still unidentified, so same-frame slot reuse is currently handled through object
  state, shot type, logical-position, and draw-position guards.

## 2026-11-11

### Added

- **Synchronized GameFPS, replay speed and multiplier display with official thprac** (`e77dfcd`)

  thprac takes over th06nc's update call and replay speed controls. When `thprac64.dll` is present,
  GameFPS now follows thprac's limiter setting, while replay fast-forward and the native `xN`
  display are left to thprac; the previous hard-coded `x8` display is gone. The patch still reads
  the game's remapped low-speed action, uses it to invoke thprac's white `/N` slowdown indicator,
  and counts updates that actually execute at the update-chain entry so the BGM can be realigned
  when slowdown ends.

  Verified with official thprac: non-default fast-forward values display correctly, remapped
  slowdown and BGM realignment work, both overlays coexist, and the game remains at 60 FPS.
  Without thprac, the existing `ReplaySlowRate`, `ReplayFastRate` and `GameFPS` paths are unchanged.

## 2026-11-10

### Fixed

- **The overlay no longer disappears when the window loses focus** (`284d165`)

  The game continues rendering and presenting while its window is in the background, but the
  overlay previously turned itself off when the foreground-window check failed. The overlay now
  continues to refresh and draw, including the Present FPS and frame-processing time readouts.
  `ShowOverlay = 0` still turns it off normally.

## 2026-11-09

### Added

- **Added a standalone x64 DLL, loader and configuration system** (`3f2f217`)

  Added `openinputlagpatch_x64.dll`, `oilp_loader_x64.exe`, SHA-256 version detection and the
  global configuration file
  `%APPDATA%\ShanghaiAlice\oilp_custom\openinputlagpatch_x64.ini`. The loader can inject after
  starting a game or attach to an already running supported process.

- **Retained GameFPS, replay slowdown, overlay, console and flip-model support in the x64 runtime** (`3f2f217`)

  `GameFPS` takes over the game's main-loop frame limiting. Holding Left Shift during replay slows
  playback by `ReplaySlowRate`, shows the corresponding multiplier in the native replay indicator,
  and realigns the BGM when the key is released. `ShowOverlay` draws the actual Present FPS and
  frame-processing time directly onto the game backbuffer without creating another window that
  could downgrade flip presentation. `DebugConsole` opens a debug console, and
  `ForceFlipDiscard` requests `DXGI_SWAP_EFFECT_FLIP_DISCARD` before swap-chain creation.

- **Retained U-key diagnostic export** (`3f2f217`)

  `oilp_diag.txt` records the x64 version, configuration, hook state, swap-chain model, replay
  slowdown state, BGM realignment, player position and the last 300 game frames.

- **Replay slowdown now reads the game's own low-speed movement action** (`2a41c58`)

  The patch no longer polls physical Left Shift. It saves the frame's live synthesized input before
  the game writes replay input into the logical state, then triggers slowdown from the low-speed
  movement action bit. In-game remapping, alternate bindings and third-party key changes continue
  to work.

- **Added the configurable replay fast-forward multiplier `ReplayFastRate`** (`2a41c58`)

  The default is `8`, matching the original game, and the accepted range is `1` through `128`.
  Holding the game's configured shoot key runs the native replay acceleration loop at this
  multiplier, and the native replay indicator shows the matching `xN` value.

### Changed

- **Temporarily retained the overlay's former interpolation-rate row** (`3f2f217`)

  The current x64 build has no interpolation, so this row reports actual Present FPS instead of an
  interpolation target.

### Fixed

- **Fixed frequent startup crashes when `ReplayFastRate` was not 8** (`2a41c58`)

  Older WER records confirm the same crash existed before the multiplier feature; it occurred while
  the overlay released D3D11 COM objects. Overlay resource lifetime is now serialized with an
  exclusive lock, pointers are cleared before release, and D3D objects are no longer released under
  the loader lock from `DllMain(PROCESS_DETACH)`. Repeated non-8 startup tests complete normally.

### Removed

- **Removed the th06nc x64 interpolation and Render Replay implementation** (`3f2f217`)

  Object extrapolation, extra frames, extra presentations, render-chain replay, capture-target
  history, D3D11 resource tracing and x64 interpolation configuration were removed. The x64 ini no
  longer reads `[Interpolation]`. Interpolation entry points that remain in the shared limiter are
  used only by the existing x86 build, so x86 interpolation is unaffected.
