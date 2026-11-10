# Changelog

[简体中文](Non-AI-CHANGELOG.zh-CN.md) | English | [日本語](Non-AI-CHANGELOG.ja.md)

## v1.14
- Added frame interpolation support for Double Spoiler (the extremely fast bullets in Spell Card EX-4 are not interpolated to prevent visually seamless bullets)
- Improved the player interpolation algorithm to eliminate the half-frame visual delay in player interpolation (when paused on some screens, the player may appear to jitter in response to the direction keys, but this has no practical effect)
- Added a feature for th19 and th20 that forces the `input method` in `custom.exe` to `Fast` when the game starts (this feature had already been added for other games in the upstream repository)
- Added a renderer API for [RUEEE's thprac](https://github.com/RUEEE/thprac) to fix incorrect GUI flickering in thprac during frame interpolation in the decimal games (compatible with `thprac-CA` and later versions)
- The fullscreen-selection prompt enabled by `AskWindowMode` in th06-th09.5 and the windowed-fullscreen option built into th10 can now be closed with X
- The game executable selection window in `oilp_loader.exe` now correctly displays executables with Chinese and Japanese names, and the game-selection prompt supports multiple languages according to the system language
- When a game crashes unexpectedly, the OILP patch now automatically creates `oilp_crash.txt` in the game's root directory to help track down bugs
- Fixed the issue where Mountain of Faith and Fossilized Wonders still used the games' own replay fast-forward mechanisms when `ReplaySpeedControl` was enabled
- Fixed the issue where replay fast-forward and slowdown did not work correctly in Undefined Fantastic Object when `ReplaySpeedControl` was enabled
- When `ReplaySpeedControl` is enabled, replay fast-forward and slowdown speeds in Fossilized Wonders can now be changed through RUEEE's thprac, and `GameFPS` is compatible with thprac's game-logic frame-rate changes (compatible with `thprac-CA` and later versions)
- Fixed cases where replay continued to play at accelerated speed after the fast-forward key was released when `ReplaySkipFPS` was set relatively high
- Fixed the bug where replay fast-forward and slowdown became extremely fast when `ReplaySkipFPS` or `ReplaySlowFPS` was less than or equal to 0; values less than or equal to 0 now use the defaults of 480 fps and 30 fps
- `ReplaySkipFPS` now defaults to 480
- Fixed the issue where th10-th15 were more likely to crash during transitions and similar situations (the actual effect is still uncertain for now)
- Fixed the issue where returning to the game after switching away while using a dgVoodoo2 exclusive-fullscreen configuration stretched a 4:3 image across the entire screen (when dgVoodoo2 is detected, the `D3D9Ex` option is forcibly disabled; because dgVoodoo2 already provides a low-latency `flip model`, this has no negative effect on latency)
- Fixed incorrect rendering of player shots near the top of the playfield when frame interpolation was enabled in Embodiment of Scarlet Devil, Perfect Cherry Blossom, and Imperishable Night
- Fixed the issue where the player was forced into the upper-left corner when frame interpolation was enabled in Embodiment of Scarlet Devil and Perfect Cherry Blossom with `PlayerBody = 0`
- Fixed incorrect display of the Cherry value in the lower-left corner of Perfect Cherry Blossom and the Spell Card Bonus when those values became relatively large; this can be toggled with `FixTH07Cherry&BonusDisplay` in the ini configuration and is enabled by default
- Fixed the score display counting backwards after exceeding a certain value in Undefined Fantastic Object; this can be toggled with `FixTH12ScoreDisplay` in the ini configuration and is enabled by default
- Fixed the crash after the post-boss dialogue when playing practice-mode replays in some games (th10, th13-th17); this can be toggled with `FixPracModeReplayCrash` in the ini configuration and is enabled by default
- Added optional forced 4:3 output in exclusive fullscreen (this modification greatly reduces image quality; if a game is not displayed in 4:3, check the graphics-card and display settings first); this can be toggled with `ForceRatio_4_3` in the ini configuration and is disabled by default
- Added an option that bypasses the broken NVIDIA Detours trampoline installed over the Direct3DCreate8 entry of older dgVoodoo2 versions, fixing dx8 games failing to start on some relatively uncommon older PCs (the issue occurs in Windows 7 + Optimus + dgVoodoo2 environments); this can be toggled with `BypassNvidiaD3D8Detour` in the ini configuration and is disabled by default
- The Payday gang has obtained the Hope Diamond
## v1.13
- Fixed the bug where Wily Beast and Weakest Creature (th17) could crash when entering the next stage with interpolation enabled (affected versions: v1.11-v1.12; players using those versions should update)
## v1.12
- Added frame interpolation support for Impossible Spell Card and 100th Black Market
- Fixed interpolation jumps in some lasers in Perfect Cherry Blossom
- Fixed an option interpolation offset under certain conditions for Marisa C in Mountain of Faith
- Improved the game detection logic; non-original games no longer simply fail to start, and instead show a warning while the patch is still applied
- Various error dialogs and the fullscreen-selection dialog now support Chinese
- Added several parameters to the U-key diagnostic export for investigating interpolation failures on some PCs
- FullscreenRefreshRate now defaults to 0 (meaning the game refresh rate follows the display's maximum refresh rate in exclusive fullscreen)
- Removed the fungus acceleration refill crystal
## v1.11
- Added frame interpolation support for Shoot the Bullet (th09.5)
- Probably maybe fixed the issue where built-in screenshots in some games capture extra frames
- Interpolation can now be toggled separately (split into enemy bullets and lasers, the player body, and player shots; all enabled by default)
- Added support for a global configuration file; when the game directory has no configuration file, "%APPDATA%\ShanghaiAlice\oilp_custom\openinputlagpatch.ini" is used as the global configuration
- Fixed the black result screen in Perfect Cherry Blossom when interpolation is enabled, as well as the abnormal fade-in/fade-out speed during every stage transition in th10-th20
- Fixed the abrupt flickering horizontal laser from Starlight Typhoon in Imperishable Night when interpolation is enabled
- Fixed the background flashing when Reisen's bullets blur in Imperishable Night when interpolation is enabled
- Fixed the bug where extra frames did not correctly restore screen shake when interpolation is enabled, significantly weakening the screen shake
- Fixed the issue where a small black rectangle would still appear when the overlay should not be shown in some cases
- Added support for the Steam versions of th18-th20
- Automatically force interpolation off when th19's third-party netplay tool, Ju.N.Owen, is enabled, to prevent unexpected issues
- Removed Steven
## v1.10
- Added support for th06-th08
- Ported many of vpatch's special optimizations for the old DX8 games (some can be configured in the ini file, but keeping the default settings is recommended)
- ini configuration change: the interpolation default is now 1; when the parameter is 1, the target interpolation frame rate automatically synchronizes with the display refresh rate
- ini configuration change: the ShowOverlay = X parameter now means X=0: show neither X=1: show only the interpolation frame rate X=2: show only latency X=3: show both; the default is X=1 (latency can also be checked with thprac, and having both overlays risks blocking the enemy marker at the bottom of the playfield)
- Removed Herobrine
- Note: during development of the DX8 games, many bugs were encountered and fixed wherever possible, but they cannot be guaranteed to be stable at present. Unexpected issues may still occur, so please be aware of this.



