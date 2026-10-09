# Changelog

English | [简体中文](CHANGELOG.zh-CN.md)

Changes made on top of upstream [OpenInputLagPatch](https://github.com/khang06/OpenInputLagPatch).

Only changes that have been tested in-game and confirmed working are listed here, so this file
is also the record of what has actually been verified rather than just what was attempted.

## 2026-10-09

### Added

- **Touhou 15's frame interpolation covers the player, its options, its shots and the straight
  lasers** (`14e0551`)

  With only the bullets moving forward, the extra presentations showed a half frozen screen: the
  bullets travelled while the player, the options beside it, the bullets it fires and the straight
  lasers the enemies put up all repeated the frame the game's own presentation had shown. None of
  them has a velocity to read back - the player moves by what the input says, the options and shots
  come after it smoothly, and a laser turns and grows as it goes - so the patch measures instead:
  every game frame, at the point where the game presents, the fields the draw actually reads are
  compared with the frame before, and the extra presentations write those differences back times
  the fraction of a frame they stand for, then put them back right after the render. Something
  that didn't move - a held picture, a paused game - comes out as zero on its own, and something
  that only just appeared, or a pool slot handed to another object, has nothing to compare against
  and is left alone for that frame.

  What is moved is what the draw reads: the player's own position at +0x618, which the render
  copies into its animation object, and the animation objects themselves (+0x5EC) for the options
  and the player's shots, whose positions are written by the update. The lasers are walked through
  their own manager's list (0x4E9BA0), and only the straight kinds are handled (the line, infinite
  and beam vtables); those carry their own position (+0x54), angle (+0x6C) and length (+0x70),
  which is exactly what their draw builds the picture from. The curve laser is deliberately left
  alone, as the requirements ask, and the manager's own sentinel entry is skipped because its
  vtable isn't one of those.

  Two things went wrong on the way and are fixed: the first cut called the game's own sprite
  lookup through a function pointer, whose argument is passed on the stack, so it took back a
  pointer that wasn't an animation object and the game crashed as soon as a stage loaded; and the
  "position of the frame before" was kept in a table that is rebuilt every frame, so every
  difference came out as zero and the player never moved. Confirmed in-game: the player's, its
  options' and its shots' sprites are tracked and move with it, and the straight lasers in play
  (12 in the test) are found and projected. The enemies are still to come - they are the last
  piece of the stage 1 work - and the curve lasers stay out of scope.

- **The overlay shows the rate the frames are actually presented at** (`bad3516`)

  With the interpolation on, a game frame reaches the screen more than once, so the game's own
  frame rate stops describing what the display is getting, and nothing said whether the extra
  frames were landing evenly - the only way to tell was the diagnostic dump. The overlay now
  carries a second line with the rate the frames are really presented at, counted from the
  presentations themselves (the game's own one and every extra one), so it shows what the display
  gets rather than what the config asked for. It only appears when the overlay and the
  interpolation are both on, since with the interpolation off it would just repeat the rate the
  game already runs at, and it is measured over a 0.5 second window so it doesn't flicker.

  The line goes at the right edge of the play area rather than in the bottom right corner of the
  window, because that corner is where thprac draws its own frame rate and slowdown readout and
  the two ended up on top of each other. The game hands the overlay the anchor in its own
  coordinates (416 out of 640, the play field being 384 wide and starting at x = 32) and the
  overlay scales it to whatever the back buffer is, so it lands beside the picture whether it is
  presented 1:1 or scaled up. Confirmed in-game: it sits just to the right of the play field,
  clear of thprac's reading, and reports about 3.0 presentations per game frame at 180/s with
  `Interpolation = -1` on a 180hz display. The anchor is th15's own play field for now; another
  game that interpolates would need its own numbers.

- **Window mode: `CoverTaskbar` keeps the taskbar out of the way without holding the window on
  top** (`7104d0e`)

  Covering the taskbar means sitting in the topmost band, because the taskbar is a topmost window
  itself - which is why `AlwaysOnTop` has always come with the game covering every other window as
  well, more than someone who just wants a borderless screen-size window without the taskbar
  needs. `CoverTaskbar` splits the two apart and only keeps the taskbar out of the way while the
  window is the active one: 1 tells the shell the window is a fullscreen one
  (`ITaskbarList2::MarkFullscreenWindow`) so it steps the taskbar aside, with the window staying an
  ordinary one so that whatever is switched to still covers the game; 2 holds the window in the
  topmost band only while it is active, the same picture as `AlwaysOnTop` while playing but
  letting go the moment another window is activated. 1 falls back to 2 when the taskbar list can't
  be had.

  The first cut hung the game before it drew a single picture, for two reasons: it took the game's
  window procedure over to watch for activation, and changing the state sends messages back into
  that same procedure (SetWindowPos sends the position and activation changes, and the shell
  answers), so it re-entered itself and never came back; and it asked the shell for its taskbar
  object on the game's thread, a cross-process call that can block for as long as the shell is
  busy, which is fatal in the middle of starting up. The state is watched by a thread of its own
  now, which looks at which window is active every 50ms, waits until the window is actually up,
  and only calls anything when the answer changes; everything that can block happens on that
  thread, and the game's own thread only starts it. Confirmed in-game. The honest limit is that
  nothing can be done while the window isn't the active one - a window that isn't topmost is below
  the taskbar by definition - so this is a taskbar that stays out of the way while playing, not one
  that is gone for good.

### Changed

- **The reference config and the READMEs are bilingual and say what the interpolation actually
  does** (`bad3516`)

  The ini's comments were English only, which is the wrong way round for the people most likely to
  read them, so every entry is written in Chinese first and English after, with the default each
  one documents aligned to its value. The example blocks in README.md and README.en.md are
  generated from the ini so all three stay identical, which is what the READMEs claim they are.
  The `Interpolation` entry now spells out that the extra frames are made by *extrapolating* -
  position += velocity times the fraction of a frame, drawn, put back - rather than by keeping the
  previous frame and interpolating between the two, and it lists what that covers so far: th15's
  bullets, the player with its options and shots, and the straight lasers, with the enemies not
  done yet and the curve lasers left alone.

## 2026-10-08

### Added

- **Touhou 15 gets frame interpolation** (`641f5ac`, `7456449`)

  The game keeps running at its own frame rate, but every one of its frames is put on screen more
  than once - 60fps logic on a 180hz display is presented at 0, half a frame and one frame - which
  is what the new `Interpolation` option (off by default) sets up. The extra presentations draw the
  state the game is already in, so they need something inside them that moves to be worth anything:
  the bullets are projected forward by velocity * the fraction of a frame the presentation stands
  for, before each extra presentation and put back right after the render. That is the movement the
  game applies to a bullet itself between two of its frames, and the game's own logic never sees
  the projected positions.

  The extras are presented with the device's own Present, taken before any tool can replace it:
  thprac's Present hook measures the input latency assuming one present per game frame, and with
  the extras it would report a fraction of a frame instead. Presenting the scene again also erases
  the overlay a tool has already drawn for the frame - thprac draws its ImGui overlay from the
  return of the object render - so the value that says "the tool's frame is built" is found at
  runtime and set again before each extra presentation, which draws that same overlay into every
  one of them without running any of the tool's own code: running it again would advance ImGui's
  key repeat once per presentation, and holding a direction key would skip through its menus
  several times too fast.

  The bullets are found through the manager the game builds for them when a stage starts - the one
  that loads bullet.anm - rather than by looking at what their sprites look like, so this is
  exactly the game's bullets: an enemy bullet isn't one of the sprite objects the rest of the
  screen is made of, it is a larger object of its own on that manager's list, with its position and
  the movement it gets per frame next to each other at another set of offsets. Confirmed in-game:
  the presentations land evenly on a 180hz display with the game's own frame rate unchanged, the
  bullets move between them (the diagnostics report every bullet in play as projected), and the
  pause screens, the menus, thprac's overlay and its latency reading all behave. The enemies and
  the straight lasers are not projected yet - they come with their own later steps - and the curve
  lasers won't be done at all.

- **Touhou 19 v1.10c is supported** (`0c8a926`)

  th19 is a D3D9 game, so it gets the D3D9 side of the patch - the overlay, the D3D9Ex upgrade
  and the frame limiter - and, like the other D3D9 games, no question about the screen mode on
  boot. It has no replay system, so there is no replay speed control for it. Its executable is
  the first one with ASLR enabled, so every address is rebased onto wherever it got loaded
  instead of being used as it is.

  Unlike the older games, th19 doesn't run its logic once per rendered frame: each update
  advances the engine's clock by one frame at a time and only runs the game's logic once that
  clock says a frame has passed. How long a frame is comes from a hardcoded 60fps that's read
  out of a few instructions, so replacing the frame limiter alone left the game running at its
  original speed no matter what `GameFPS` was set to. Those instructions read the patch's frame
  rate now, which is what thprac's FPS option does for th11 and up.

- **Touhou 20 v1.00c is supported** (`18e1b2f`)

  th20 runs on the same engine as th19, so the same support applies to it. Unlike th19 it has a
  replay system, so it also gets replay speed control: holding the shoot key fast forwards a
  replay and holding the slow key slows it down. The shoot key is th20's own replay fast
  forward - holding ctrl does nothing in these games, which is how th17 and th18 behave too -
  and the frame rates used are `ReplaySkipFPS` and `ReplaySlowFPS`.

### Fixed

- **th19 and th20 had a whole frame of extra input lag, and the overlay reported the wrong
  time** (`18e1b2f`)

  Both games pace their frames with a clock of their own instead of calling their frame limiter
  from the gameplay path, and the limiter was ticked from the D3D9 EndScene hook, which runs
  before the frame is presented: the wait landed between the input being read and the picture
  being presented, so thprac's latency display showed a full frame (~16ms) instead of the ~1ms
  the game needs.

  The wait happens right where the game itself waits now - after the previous frame was
  presented, before the input is read - and it uses the time the game has already worked out:
  right before its own `Sleep(1)` the code has the time left until its clock's next deadline,
  so the wait ends exactly when that clock says the frame is due and the two can't drift apart.
  The wait is precise where the game's own `Sleep(1)` wasn't, which is also where the occasional
  2.5ms latency spikes came from, and the overlay reports how long the game's own work took -
  the same thing thprac shows - instead of the whole frame. `BltPrepareTime` doesn't apply to
  these two games any more: the wait is the game's own clock and not a schedule of the patch's.

- **Missing out on a multiple of 60hz display mode no longer quits the game** (`0c8a926`)

  With `FullscreenRefreshRate = 2` (the default) a fullscreen resolution that has no refresh
  rate that's a multiple of 60 ended in a panic message that killed the game. It lets the
  runtime pick a mode for it instead, the same way the D3D8 path already did.

### Changed

- **An external tool's frame rate wins over `GameFPS` on th19 and th20** (`18e1b2f`)

  Both games run their own clock off a frame rate, and thprac points that at a variable of its
  own for them: its modules for these two games don't look for the patch at all, so instead of
  handing its frame rate over through the patch's API it patches the engine's clock itself. The
  patch takes the operand back when that happens, because the replay speed control needs to own
  the clock, but it reads that variable and follows its value - so an external tool's frame rate
  still applies, and takes priority over the configured `GameFPS`.

  Replay speed control always goes by the patch's own `ReplaySkipFPS` and `ReplaySlowFPS` on
  these two games, since thprac's replay sliders can't reach the patch there; they can be set in
  the ini instead.

### Hotfix

- **th06 to th18 ran at high speed in v1.6** (`5ea9c18`)

  The deduplication the limiter got for th19 and th20 treated a tick that arrived sooner than a
  quarter of a frame as one belonging to the frame that was already being timed and skipped the
  wait for it. That holds for th19 and th20, which keep a clock of their own and are only ticked
  after their own wait - but not for th06 to th18, where the limiter is the one doing the
  waiting: there the tick measures the game's own work, which is usually only a millisecond or
  two, so most frames were not waited out at all, and those games ran at high speed with
  `GameFPS` doing nothing.

  th19 and th20 tick through `Limiter::TickUntil` and never had that check, so they were
  unaffected. The check is gone again, and the older games are back to their configured speed.

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
