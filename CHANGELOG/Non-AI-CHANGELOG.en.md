# Changelog

[简体中文](Non-AI-CHANGELOG.zh-CN.md) | English | [日本語](Non-AI-CHANGELOG.ja.md)

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



