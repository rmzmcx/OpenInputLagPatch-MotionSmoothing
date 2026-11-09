# x64 Changelog

[简体中文](CHANGELOG_x64.zh-CN.md) | English | [日本語](CHANGELOG_x64.ja.md)

This file records features and changes on the standalone x64 branch. It is maintained separately
from the changelogs for the x86 games.

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

### Changed

- **Temporarily retained the overlay's former interpolation-rate row** (`3f2f217`)

  The current x64 build has no interpolation, so this row reports actual Present FPS instead of an
  interpolation target.

### Removed

- **Removed the th06nc x64 interpolation and Render Replay implementation** (`3f2f217`)

  Object extrapolation, extra frames, extra presentations, render-chain replay, capture-target
  history, D3D11 resource tracing and x64 interpolation configuration were removed. The x64 ini no
  longer reads `[Interpolation]`. Interpolation entry points that remain in the shared limiter are
  used only by the existing x86 build, so x86 interpolation is unaffected.
