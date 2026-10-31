# Changelog

[简体中文](CHANGELOG.zh-CN.md) | English | [日本語](CHANGELOG.ja.md)

Changes made on top of upstream [OpenInputLagPatch](https://github.com/khang06/OpenInputLagPatch).

Only changes that have been tested in-game and confirmed working are listed here, so this file
is also the record of what has actually been verified rather than just what was attempted.

## 2026-11-01

### Added

- **Impossible Spell Card (th14.3) gets frame interpolation** (`61b5021`)

  th14.3 has its own three scene frame functions and Present sites. The patch takes them over so an
  extra presentation replays only the scene render and bypasses the original Present setup. Logic,
  timers, input, replay state, screenshots, item cards and stage scripts stay on the game's own
  frame. The extra pass saves and restores the scene viewport, transforms, selected D3D states,
  texture, camera and sprite-queue position. Menu text record counts are restored before each extra
  render, so text is not appended or drawn twice.

  thprac combines its GUI update and draw in one EndScene hook in this game. Re-running that hook
  advanced ImGui and made both the tool and game picture flicker. The patch now calls the verified
  C9 DX9 backend in draw-only mode, after checking its RVAs and machine-code signatures. If a
  different tool build is loaded, the overlay replay is skipped safely.

  Enemy bullets are projected from the game's own in-play manager chain using the state-dependent
  movement factors, the per-render update count, slowdown scale and pause flags. Player movement is
  measured from the visible logical position. The eight option slots and the 256 player-fire
  registry slots are resolved to their real sprite objects, with their drawn positions measured,
  projected and restored. Line and infinite lasers are measured over their position, angle and
  length, with angle differences folded across the +-pi boundary. LaserBeamInf has a stub draw and
  LaserCurveInf remains excluded as required.

  Checked in game at 180 presentations per second: dense enemy bullets, player movement, player
  shots and straight lasers all move across the extra presentations. The final laser test recorded
  92 of 92 active Line/Infinite lasers projected, three presentations per logic frame and about
  180.6 presentations per second. Pause, menus, loading screens, stage changes, screenshots and
  replay updates are outside the repeated path because only the scene render is replayed.

## 2026-10-31

### Added

- **100th Black Market (th18.5) gets frame interpolation** (`ce01d17`)

  th18.5 uses the th18 family's task and render model, but its player fire is created from ability
  cards at runtime. The main-shot and companion sprite handles, and the visible card-fire objects,
  change with the active loadout, so th18's fixed shot offsets cannot be reused. The patch now takes
  over the render and present points in both frame functions. The game's own frame keeps logic,
  timers, input, screenshots, replay state and card updates to itself; an extra presentation replays
  only the render chain and restores menu-text counts, stage-transition fields, viewport and sprite
  placement. Fade creation is blocked while that render walk is repeated.

  Enemy bullets move from their six manager lists using the state-dependent velocity factor. The
  player's visible logical position drives its embedded animation object. Straight and infinite
  lasers are measured per frame over position, angle and length. Card firepower first covers the
  ability manager's main and companion sprites, then includes drawn sprite objects that actually
  moved that frame. These records are deduplicated against the bullet, player and laser paths so no
  object is advanced twice. Checked at 180 presentations per second: enemy bullets, the player,
  the special main-shot/card firepower and straight lasers all move across extra presentations,
  while pause, menu text, loading black screens and stage changes stay on the game's logic frame.

- **Presentation diagnostics and `U` export to `oilp_display_diag.txt`** (`d810ebb`)

  The diagnostic records monitor and DWM refresh rates, the effective D3D8/D3D9 presentation
  parameters, the last 600 present intervals and how long Present blocks. This separates frames
  the patch never submitted from frames it submitted but could not show at the requested time.

### Fixed

- **Startup messages are localized and modified games remain identifiable** (`f824ac9`)

  Every startup and D3D initialization message now chooses Chinese or English from the Windows
  display language or system locale. Executable hashes remain authoritative. When a hash is
  unknown, the filename is scanned for a supported `th` plus number token, so renamed variants such
  as `th15_n.exe` can still start; compressed work numbers are mapped back to names such as th09.5,
  th12.8 and th18.5 before showing the non-original warning.

  Startup warnings used to run inside `DllMain` while the loader lock was held, where nested
  message loops could occasionally leave the dialog unresponsive. Messages are now queued and shown
  from the first game-thread `CreateMutex` or `CreateWindowEx` call. Unsupported games exit only
  after the user acknowledges the error.

- **The present-rate overlay no longer shows `0.0 fps` before a measurement exists** (`ce01d17`)

  The D3D8 and D3D9 overlays used to draw `0.0 fps` during the first half-second sample, or for a
  game that never counted a presentation. The line is now drawn only after a real measurement above
  zero exists. th18.5 also uses the same bottom-left overlay layout as the other interpolated games.

## 2026-10-30

### Fixed

- **Perfect Cherry Blossom's rotating lasers use the shortest arc and reject scripted jumps**
  (`95a79e3`)

  th07 stores ECL-updated laser angles wrapped to the interval from -pi to pi. Crossing that
  boundary made the raw frame difference almost 2 pi, so an extra presentation swept a small turn
  across the screen and then snapped back. The measured step is now folded to the shortest arc and
  the projected angle is wrapped back to the same interval. An ECL assignment that sets the angle
  outright or aims the laser at the player is a scripted state change, not continuous motion, so
  angle steps above 45 degrees are no longer interpolated. Checked in game on Great Funeral
  Ghostly Wheel Concerto Grosso Monster and Resurrection Butterfly -80 Percent Reflowering.

## 2026-10-27

### Fixed

- **Marisa C's options stay put while focus hold freezes them in Mountain of Faith** (`37c60a6`)

  Marisa C freezes her options while focus is held and keeps them frozen briefly after release,
  until the return movement begins. The old code still projected the held frame by the movement
  measured before the freeze, which made the options jump forward when focus was toggled quickly.
  Only that held frame's measured option step is now treated as zero; fast movement and the delayed
  return remain interpolated.

## 2026-10-24

### Changed

- **Fullscreen refresh rate defaults back to the game and display's normal selection** (`d810ebb`)

  `FullscreenRefreshRate` no longer defaults to forcing the highest refresh rate. The old default
  could force a game down to 60 Hz when its chosen fullscreen resolution had no suitable multiple of
  60. The default is now `0`; an explicit ini value still overrides the game's selection.

### Documentation

- **The complete `GameOverride` table is documented** (`2b7968b`)

  The Chinese, English and Japanese READMEs now list every value, keep hash autodetection as the
  recommended mode, and explain that a wrong override can apply a game's addresses to an unrelated
  executable. The Shoot the Bullet example now describes forcing the patch onto a modified build.

## 2026-10-23

### Added

- **Shoot the Bullet has frame interpolation** (`851a9ff`, `cd408d8`, `31fe6d5`)

  th09.5 keeps its calculation, drawing and photo work on one frame path. Reusing that path for an
  extra frame would advance timers and could make a second screenshot request, so the game frame
  remains the only logic frame and extra presentations use the device Present after it. Enemy
  bullets are measured from the game's 1600 fixed slots and six layer chains; straight lasers are
  measured from the game's LaserInf list and the position, angle and two visual scale fields of its
  two concrete classes. PlayerBody covers the player and the four child/viewfinder components
  through the five positions maintained by the player's camera subobject. th09.5 has no player-shot
  object group, so PlayerShots intentionally moves nothing.

  The screenshot request and `snapshot/th###.bmp` write stay in the game Present tail before an
  extra can run. The shutter, HUD, score text, photo strip and BOSS/enemy bodies are not moved.
  Extra draws save the live post-Present scene, restore the state captured before the game's own
  draw, and put the live state back after the repeat. The user's 300-frame diagnostic recorded
  three presentations for every in-stage frame at 5.56/11.11 ms spacing, 60.01 fps in the game's
  own counter, a zero lag ratio, 101/101/101 enemy-bullet counts and zero photo-state changes.

### Fixed

- **th09.5 startup avoids the benchmark window flash, and thprac stays valid across resets**
  (`851a9ff`)

  Removing the original limiter made the first-launch benchmark select forced-60 mode and restart
  the window. The patch now selects that mode before device creation and keeps the first game
  window hidden until a frame has actually been presented. thprac's th095 module has one combined
  GUI update/render hook, so extras draw its cached frame data instead of running the update again.
  Reset also invalidates the render backend; the game now reinitializes thprac after a successful
  D3D8 Reset. The D3D8 overlay restores its full state block and separately restores the viewport,
  which a state block does not contain.

## 2026-10-20

### Fixed

- **The stage clear transition keeps the game's own speed in th10–th20** (`45d099e`)

  th10 to th20 advance part of the stage transition from draw callbacks: one draw callback subtracts
  one from the stage clear timer on every render pass, and another creates the 30 frame fade when the
  timer crosses 30. The game draws once a frame, so that is one step per frame; an extra presentation
  ran the render chain again, which made the stage clear sequence run two or three times as fast at
  120/180/300 fps and could create more than one fade for the same game frame. The fade objects
  themselves were fine: their own timers run on the update chain, once a frame.

  An extra pass now saves the stage object's transition fields and restores them after its render
  walk, and skips the fade creation while it is drawing. Only the transition fields are copied:
  restoring the whole object put back pointers the draw had replaced, which crashed th16 and th18
  when the next stage loaded. th16 and th18 also do not check their inline fade allocation for null,
  so their creation branch is trampolined instead of making the allocation fail.

  The fade creation sites and stage sizes: th10 `0x402A61`, `0x2B64`; th11 `0x40335B`, `0x3158`;
  th12 `0x40391D`, `0x3724`; th13 `0x406FA8`, `0x41C0`; th14 `0x40E513`, `0x43A0`; th15 `0x40EA1A`,
  `0x6788`; th16 branch `0x40A1B2`, `0x66F0`; th17 `0x40A77D`, `0x67B8`; th18 branch `0x41C488`,
  `0x34A0`; th19 `0x4EC75B` (seven argument fastcall), `0x3638`; th20 `0x472FE8` (cdecl), `0x336C`.

- **th08's rotating lasers interpolate along the shortest arc** (`9adc923`)

  The laser angle is stored wrapped to +/-pi (the ECL opcode at `0x41BDA5` normalises it with
  `FUN_0043EDB0`), so crossing the boundary produced a raw frame difference of about 2pi. The extras
  interpolated that difference directly, which made a horizontal laser flip for one extra
  presentation (clearest in 4B's Starlight Typhoon). The angle step is now folded to the shortest
  arc and the projected angle is folded back:
  `theta = Wrap(theta1 + t * Wrap(theta2 - theta1))`. Scripted `startOffset`/`endOffset` jumps are
  not motion and are still not interpolated.

- **th08's stage 5 illusion keeps its background tint** (`9adc923`)

  The stage 5 boss's illusion sets a background tint (`g_Background` at `0x4E4030`, colour at
  `+0x6468`, enabled at `+0x646C`) through `FUN_00409160`; the background draw applies it and then
  resets the stored colour to grey. An extra pass therefore applied the grey default instead of the
  illusion tint, so the background flashed between the two. The tint state the game's own pass
  started with is now put back around every extra pass, and the state that pass left is restored
  afterwards.

- **Overlay readouts no longer depend on interpolation** (`1967235`)

  `ShowOverlay=1` means the present-rate line by itself. The empty render-time line used to leave a
  two-pixel background in the corner, but skipping that background also skipped the stream, index,
  FVF and font-texture bindings that the rate line relied on, so the overlay disappeared when
  interpolation was on. Both the D3D8 and D3D9 renderers now bind their shared pipeline before
  either line and draw each line's background only when that line has text. All four `ShowOverlay`
  values now behave independently of the interpolation switch, with no stray black pixel when the
  rate line is unavailable.

### Added

- **Interpolation targets can be selected independently, with global defaults** (`4539e38`)

  The interpolation master switch could only turn every extrapolated object on or off. A new
  top-level `[Interpolation]` section keeps `Interpolation` as the master switch and adds
  `PlayerBody`, `PlayerShots` and `BulletsAndLasers`, all on by default. The player switch includes
  options, orbs and familiars; the other two split the player's shots from enemy bullets and straight
  lasers. Each game already had separate advance and undo paths for these groups, so the switches
  gate the existing measured extrapolation rather than introducing another rendering or logic
  model. Old `[Option] Interpolation` files remain readable.

  The patch also creates and loads
  `%APPDATA%\ShanghaiAlice\oilp_custom\openinputlagpatch.ini` as the global default configuration,
  then overlays only the keys present in the game directory's ini. The generated global file is
  embedded from the shipped bilingual reference, so it keeps the full comments without silently
  replacing an existing user file.

## 2026-10-18

### Fixed

- **The screen shake stays in the extra presentations** (`9786d72`, `13eaa9c`, `8f150b0`)

  Eleven games set a screen shake in their calculation chain, read it while drawing and clear it
  *inside* their own draw pass. That is correct for a game that draws once per frame, but an extra
  presentation starts from the cleared value, so a frame presented three times showed one shaken
  picture and two still ones and the shake read as a fraction of its amplitude (clearest in th11,
  th13 and th15 to th18). The value the game's own pass started with is now put back around every
  extra pass, and the value that pass left is restored afterwards: th08's
  AnmManager::screenShakeOffset at 0x1C, th10's 0x491E64, th11's 0x4C379C, th12's 0x4CEE04, th13's
  0x4DCBE8, th14's 0x4D93B8 and 0x4D9610, th15's 0x4E7E90, th16's 0x4C1788, th17's 0x4B6198,
  th18's 0x4CD574 and th20's two background-layer pairs. Each one was found by comparing the
  vertices the game's own pass and an extra pass hand to the device and diffing the writable data,
  then checked against the game's own code (the function that writes it, the drawing paths that read
  it and the place that clears it). th06 needs nothing (its shake moves the play field rectangle,
  which the draw pass never clears) and th19 needs nothing either (its shake is set in the update
  and only cleared when a scene is set up).

- **Touhou 7's stage transition pictures are no longer black** (`9786d72`)

  The picture that shrinks into the corner at the end of a stage and the quads that scatter on the
  results screen are both drawn from a screenshot the game takes itself - and it takes it after its
  own present, out of the back buffer. That works because it draws *before* running the calculation
  chain, so the buffer still holds the last frame the player saw; this patch has to run the
  calculation chain first, so the same request was spent after the stage-clear background had filled
  the screen and the screenshot came back black. The pending screenshot is now taken right after the
  calculation chain and before the frame's draw, which is the same image the game would have taken.
  The extra passes also put back both copies of the view and projection matrices, so they start from
  the state the game's own pass started from.

### Added

- **The Steam releases of th18, th19 and th20 work through the loader** (`8407a02`, PR #2 by es2z)

  Those executables are wrapped with SteamStub, so their code is still encrypted when the DLL is
  loaded and patching then would be scrambled when the stub decrypts the section. The DLL now spots
  the stub by its `.bind` section and delays the patches: it hooks a few `kernel32` imports that the
  CRT calls right after the handover and installs everything on the first call that comes from the
  game's own `.text`. The loader sets `SteamAppId`/`SteamGameId` before starting the game (otherwise
  SteamStub relaunches it through Steam and the injected DLL is lost), and runs the game to its
  entry point, holds it there with a `jmp $`, injects while it is stopped, then restores the bytes
  and resumes. Injecting into a process whose DLLs have not been loaded yet makes the injected
  thread load them, which hangs Ju.N.Owen's `d3d9.dll` on th19. The th19 v1.10c and th20 v1.00c
  Steam executables are added to the hash table; their decrypted code matches the non-Steam
  releases.

- **th19 v1.00a and Ju.N.Owen are supported** (`6bc1298`, `ef8f55a`, `3ab5a04`, PR #2 by es2z)

  v1.00a shares no addresses with v1.10c, so the frame limiter, the `Sleep` call and the D3D9 call
  site were matched by what the functions do. Its clock reads a frame *period* (a double holding
  1/60 at 0x587518) instead of working out 1.0/60.0, so those reads are redirected to a period the
  patch keeps at 1/GameFPS; interpolation stays off because the object layout has not been checked
  on this build. Ju.N.Owen ships a `d3d9.dll` that only exports `Direct3DCreate9`, which made the
  patch DLL fail to load when it imported `Direct3DCreate9Ex` statically - the functions are now
  looked up at runtime. When that proxy is present, its `Direct3DCreate9` is called with the
  system's one redirected for the duration, so it wraps the D3D9Ex object the patch created and
  keeps drawing its lobby. Interpolation is forced off when `modules\th19_junowen.dll` exists,
  since netplay steps both sides frame by frame and the extras only add work. The author checked
  the Steam releases and the module loading, and played a th19 v1.10c match against one opponent
  without a desync; the other versions were not played online.

- **Japanese README** (`807f86f`, PR #2 by es2z)

  `README.ja.md` is a translation of the Chinese README, and the language bar at the top of the
  Chinese and English ones now links to it.

## 2026-10-17

### Added

- **Touhou 6 gets frame interpolation** (`fecb43a`, `279518d`, `398e2ab`, `ca3331c`)

  Embodiment of Scarlet Devil runs on the same earlier D3D8 engine as Perfect Cherry Blossom and
  Imperishable Night, where a frame is one GameWindow::Render (0x4206E0): the window update, the
  calculation chain (the patch hooks it at 0x420853 and moves it ahead of the drawing), the drawing
  block (0x420720..0x420807) and one present (0x420A01 / 0x420A0B, both plain calls to the static
  GameWindow_Present, 0x420B50). The extras have the shape th07 and th08 gave this engine: after the
  game's own present, the render block runs once more, spread over what is left of the frame.

  What took the work this time was making an extra presentation start from exactly the state the
  frame's own pass started from. Four things turned up, all of them this engine's own traps (each
  with its evidence in the source comments): the **frame rate accounting** (DrawFpsCounter, 0x423F3F,
  adds one to a counter every pass and works the game's own frame rate out of it every half second,
  accumulating it into unk1b4 / unk1b8 while the game is in a menu - the ratio of the two is the
  processing rate the results screen shows and a replay records) has to be saved and put back around
  every extra pass; the **AnmManager's whole set of current* caches** has to be forgotten, because
  this game checks the texture *inside* the "the sprite changed" branch, so a sprite whose pointer
  repeats skips the bind as well - and the game's own pass ends by setting the device's texture to
  NULL, so the extras drew untextured white quads (which is what the first test saw as bullets and
  lasers washed out); the **viewport and the camera** exist twice each and both copies have to be put
  back (g_Supervisor.viewport is where the play field's viewport and the stage camera both come from,
  g_Supervisor.viewMatrix / projectionMatrix are what Stage::RenderObjects culls and projects the
  stage with while the device holds its own pair, and the pass rewrites both partway through); and the
  **depth buffer** has to be cleared, because the calculation chain clears it once a frame and a pass
  that runs after the frame's own one otherwise inherits its depth values - which hid the things a
  pass draws first (the stage background, the circles drawn before an enemy's own sprite) behind the
  HUD and the sprites drawn last, showing as those flashing at the presentation rate. With all four
  in place the three presentations of a frame are identical.

  The objects go down three paths: the **bullets** have their position moved directly,
  `pos += velocity × the state's step × effectiveFramerateMultiplier` (steps 1, 1/2, 0.4, 1/3 and
  1/2 - note the multiplier, which th07's bullets do not have); the **player, its two orbs and its
  own shots** have no velocity to read and follow th08's measured way instead (the position is
  sampled once a frame, the difference is the frame's step, and the extras move by a fraction of it
  and put it back afterwards) - the shots are drawn from their sprite's own position rather than
  their logical one, so only that is moved and the logical position is not touched at all; and the
  **straight lasers** are measured the same way, over seven fields (position x and y, angle, both
  offsets and the sprite's two scales), which covers a laser that is growing, thinning out, turning
  or fading. The **orbs are this game's familiars**: they appear from power 8 on, a focus state
  machine places them between "24 pixels out on each side" and "8 out, 32 up", and the shots with a
  spawnPositionIdx fire out of them. The measurement of the player and the orbs carries a "a frame
  cannot move this far" guard, because a death puts the player at the respawn point in one step of
  several hundred pixels: without it the extra presentations of that frame drew the sprite on the way
  down to it, which read as a flash just below the respawn point (confirmed fixed in game).

  The menus' and the pause screen's text needed **no change** - the string list is drawn and emptied
  inside the same pass, and the draw callbacks don't append to it (confirmed in game). The
  diagnostics file (oilp_diag.txt) is the one the other games have, including the two lines that
  check the object counts against the game's own counters.

  Checked in game (180Hz, Interpolation = -1): three presentations per frame 5.56ms apart, 176 to 180
  a second; the game's own frame rate still reads 60 with its effective frame rate multiplier at
  1.0000; 352 of 352 bullets projected (the manager's own count reads 359, the difference being the
  bullets the update turns off that frame); 30 shots in play (14 flying, 16 exploding) all projected;
  8 of 8 lasers projected; death and respawn, the menus and the loading screen all normal. Not done,
  as the requirements say: the enemies themselves and the curve lasers.

- **`ShowOverlay` chooses which of the overlay's readouts are shown** (`500bafd`)

  The overlay has two lines and they answer different questions: the **rate the frames are actually
  presented at** (the one that matters while the interpolation is on) and the **render time line**
  ("0.65/2.00ms": how long the game's own part of the frame took, and what BltPrepareTime is set to -
  the line that option is tuned with). The value is now their sum - 0 neither (no overlay at all),
  1 the rate, 2 the render time, 3 both - and the default is 1. Which readouts show is the setting's
  business rather than the interpolation's, so it is decided once in Limiter::Initialize and the
  per-game calls of th06 to th20 (on both the D3D8 and the D3D9 side) are gone.

- **`Interpolation = 1` means "the display's own refresh rate" and is the default** (`500bafd`)

  1 used to mean "x1", which is the same as switching the interpolation off, so nothing used it. What
  the setting is for is how often a frame reaches the display, so 1 now asks for the display's own
  rate: a 144hz display with 60fps logic is presented 144 times a second (2 and 3 presentations a
  frame in turn, with the fraction carried from frame to frame). -1 is still the largest whole
  multiple of the game's frame rate the display can show (120 on that same display, with evenly
  spaced presentations), and N and *R are unchanged. The ini and the READMEs say so, and the default
  is now 1.

### Changed

- **The interpolation's two pieces of arithmetic live in the limiter alone** (`500bafd`)

  Limiter::PresentRate() (the presentation rate) and Limiter::ExtraPresentations() (how many extra
  presentations a frame gets and how far apart) only depend on the presentation rate, the game's
  frame rate and the current frame's length - nothing about any one game. When th07 and th08 moved
  their copies of them into the limiter, the eleven copies th10 to th20 still had (a present-rate
  function each, and an inline "presentations owed" loop each) were left in place; they are gone now,
  all pointing at the shared ones. At the same time th10 to th20 gained the guard th06 to th08 have
  had since their stage 0: while the target frame rate is above the display's (replay fast forward,
  an external tool speeding the game up) and the patch is presenting every Nth frame, the extras are
  skipped as well, instead of presenting anyway and undoing that thinning.

### Fixed

- **A D3D9 game with Interpolation = 1 did not interpolate at all** (`500bafd`)

  Those eleven copies did not know the new meaning of 1 and treated it as "x1 = off" (falling into
  `multiplier < 2` and returning 0), so no extra presentations were made and the overlay's rate
  readout said 60. Merging them into the shared implementation fixed it - and is an example of what
  leaving one game with its own copy of something costs.

- **The frame rate CalcFPS hands the game no longer counts a stall as lost frame rate** (`c839ac8`)

  With CalcFPS on, Touhou 6, 7 and 8 are given the limiter's own measured frame rate, and that was
  worked out as "frames divided by the wall clock time of the window": a frame the game spends
  stalled inside itself (loading a stage, a scene change) makes the window long while holding almost
  no frames, so it read as a slow one and the game recorded a little lost frame rate - which is the
  **processing rate** it shows on the results screen and records in a replay, a few percent instead
  of the 0.00% vpatch's CalcFPS=1 gives. The window is now counted in frames, with a frame charged
  for one frame's worth of time rather than for however long a stall made it, so a run that never
  fell behind reads the rate it was paced at (0.00%) while a machine that genuinely cannot keep up
  still reads low. Measured: the diagnostics went from 10308/10680 (+6.97%) before to the two numbers
  being equal, and the results screen now reads 0.00%.

- **The rate readout sits at the left edge on its own, and is not shown at all with the interpolation
  off** (`500bafd`)

  The rate used to be placed after the render time line unconditionally (`x = width of the text x 7 +
  8`), which left a gap even when that line was not drawn; it is now at x = 0 when there is no line
  before it. And since the rate only means something while frames really are presented more than once
  per game frame, it is not shown with the interpolation off (the render time line is unaffected - it
  is what BltPrepareTime is tuned with).

Left to do: frame interpolation for Touhou 9 and 9.5, and the enemies and the curve lasers are out of
scope as the requirements say. The D3D9 side has no Present hook of its own, so the "present every
Nth frame" thinning itself does not apply to the D3D9 games (their extra presentations do skip it
now); the D3D9 side also still forces D3DSWAPEFFECT_DISCARD and a back buffer count of 0 for D3D9
titles, which is left alone until a game gives a reason. Known approximation on Touhou 6: the
spellcard background's script is set up again by an extra pass, which can cost a frame of that
background's animation.

## 2026-10-16

### Added

- **Touhou 7 gets frame interpolation** (`a9379ec`, `7659460`, `90eccee`, `c265bce`)

  Perfect Cherry Blossom runs on the same earlier D3D8 engine as Imperishable Night, where a frame is
  one GameWindow::Render (from 0x434718): window update, calculation chain (the patch hooks it at
  0x4347DF and moves it ahead of the drawing), the drawing block, one present (0x434A0E / 0x434A18).
  The extras have the shape stage 0 gave this engine: the render block runs once more after the game's
  own present, spread over what is left of the frame.

  What is in the game goes down one of two paths. The bullets have their own position moved
  (`pos += velocity × the state's step`, the five step sizes being the ones Touhou 8 uses: 1, 1/2,
  0.4, 1/3, 1/2 - and while time is stopped only the bullets that are already flying move). The
  player, its two options and its own shots have no velocity to read - the options follow the player,
  and its shots are placed onto an option or the player by their scripts - so these are measured the
  way Touhou 8's are: their positions are sampled every frame, the extras move them by that step
  times the extra's fraction of the frame, and everything is put back the moment the render is done.
  A slot that jumps further than it could in a frame is dropped rather than moved, which is what keeps
  a reused slot from showing up as a jump. The straight lasers are measured the same way (their
  position, angle and two end offsets).

  One more thing this game shares with Touhou 8: the extras have to be kept out of the frame rate
  accounting in the draw chain (Supervisor::DrawFpsCounter, 0x4390A5, counts the passes), so
  Th7FpsState is put back around every extra presentation and the game still reads 60. The menus and
  the pause screen needed no change at all (AsciiManager::OnDrawMenus, 0x401970, draws its strings and
  clears them inside the same pass), so that stage ended with no code behind it.

- **New CalcFPS option: replace the frame rate the game measures for itself** (`7af4968`)

  EoSD, PCB and IN add one to a counter every frame, divide it by the elapsed half second, and use
  that reading to decide how much of the half second was lost; the lost part over the frames counted
  is the processing rate the results screen shows and the value a replay records
  (lagNumerator/lagDenominator in Touhou 8, framerateMultiplier/fpsAccumulator in Touhou 7,
  unk1b4/unk1b8 in Touhou 6, both read by the results screen and by SaveReplay). One half second
  window landing on a load or a scene change drags the whole run's reading with it, which is why a
  short stage or time spent between stages shows a few percent. vpatch's CalcFPS=1 sidesteps that
  measurement by taking the game's own frame rate function over, which is why a vpatched game reads
  0.00% there. This adds the same option (default 1, as in vpatch), but hands the game the frame rate
  the limiter measured for itself, so a machine that really drops frames still shows it instead of
  being pinned at zero. The counting lives in Limiter::Tick, which is reached once per game frame and
  never for an extra presentation, so the extras of frame interpolation can never end up in this
  reading.

### Fixed

- **Saving a replay crashed: the input patch read an address where the game read a flag word** (`23c5b6a`)

  The port of vpatch's input handling re-does the game's own "load the flags word into eax" at
  0x442CD9 (PCB) and 0x452319 (IN) in inline asm, written as `mov eax, dword ptr [0062F648h]` - which
  this compiler assembles as the number itself (`mov eax, 62F648h`, `B8 …` rather than the `A1 …`
  load). The value handed to the game's own "am I in a stage" test was therefore a constant: in PCB
  that constant has bit 2 clear, so the recorder's own test failed on every frame and it never
  recorded anything. Nothing else writes the end pointers a stage's replay data is measured with,
  and StopRecording only fixes the input pair up, never the fps one, so saving a replay sized the fps
  block as `stageFpsDataPointers[i] - stageFpsData[i].data` - 0 minus 0x19082400 - and the `rep movs`
  at 0x443FCA copied ~970 million dwords out of a 1 MiB buffer (the 0xC0000005 at 0x43FCA in the
  event log). Imperishable Night has the same line, but its constant happens to have bit 2 set, so
  there it only meant the game was told the wrong flags. Both now read the field in a C function
  (th7_game_flags / th8_game_flags), and the note left in the source is the lesson: an absolute
  address inside an inline asm memory operand is not a load with this compiler.

- **Touhou 8's processing rate was being changed by the extra presentations** (`6a0916a`)

  Supervisor::CalculateFps (0x446F53) is a draw chain callback, so each extra presentation ran it
  once more, and the state put back around them was missing lagDenominator (0x17CE8EC): it is the
  half that goes up 60.0 on every pass while the numerator only gets its share when a window closes,
  so the ratio every reader divides was pushed about three times off. Touhou 7 never had this
  because it saves the whole pair. Found by listing every absolute address that function stores to;
  after the fix a dump taken in a stage reads "numerator 16080.00 denominator 16080.00 ratio 1.0000".

- **Touhou 8's bullets: the time stop gate and the per state step sizes** (`538d885`): the projection
  used one step size for every state and did not copy the game's own "while time is stopped only the
  bullets that are flying stay still" (0x431490 reads g_GameManager.isTimeStopped). It now uses the
  five step sizes 1, 1/2, 0.4, 1/3 and 1/2, and projects nothing for a bullet that is flying while
  time is stopped; the same guard was added on Touhou 7's side (Sakuya's bomb).

## 2026-10-15

### Added

- **Touhou 8 gets frame interpolation** (`b16284a`, `1cb0da0`, `6966ba0`, `4f95ade`, `0479b21`)

  Imperishable Night runs on the earlier engine, where a frame is one call into GameWindow::Render
  (`0x441E70`): the input is read at the head of the calc chain (`0x441F48`), the whole frame is drawn
  in one block (`0x441FB4..0x442027`) and presented once (`0x44203D`). The patch paces that frame from
  the window update hook, so the extra presentations go between the game's own present and the next
  frame - the shape th19 and th20 have. What is new here is that the rate and the number of extras are
  worked out in the limiter itself; the per-game copies th10 to th20 each carry are left alone.

  What this game needs on top of that shape is that almost everything it draws is drawn from state the
  draw pass itself writes: the player's sprite is placed in its VM from the player's own position while
  that sprite is drawn (0x44D568..0x44D5B9), and the shots and the familiars the same way. So the
  extras move the player at the call that draws it, and move the shots' and the familiars' own
  positions before the pass - the way the bullets work, which the game's own movers also move by
  adding a velocity (`pos += velocity`, 0x410A70). The player's sprite draw turned out to be shaped
  like the AnmManager's own draws: the manager in ECX and the VM as the one argument the caller
  pushed, which the function takes off with `ret 4` (0x462FF0). Hooking it as anything else left it
  drawing from whatever ECX held, with the stack four bytes off, and the player's sprite disappeared
  outright - which is why that hook is written out by hand, and why the source now says to read a hook
  target's prologue and its `ret` before taking it over.

  The player's own shots, the familiars and the straight lasers have no velocity to read (the shots
  and the lasers are driven by scripts, the familiars follow the player), so how far each of them
  moves in a frame is measured in the game's own pass and the extras project by their fraction of it -
  which also means nothing that is not moving can be moved. A slot the game reuses for a new shot or
  laser between two samples is not projected across the jump, since that reads as a step no shot
  could take in a frame; the misalignment that showed up in the player's shots during this round was
  exactly that. The familiars are four slots whose draw callback is set at runtime, and it is the same
  function for every team (all five of them were checked), which is what the diagnostics name.

  The menus and the pause screen needed nothing at all: this game's string list is drawn and cleared
  inside the same draw pass (the low priority ascii pass runs OnDrawLowPrioImpl and then
  ResetStrings), with the HUD appending to it earlier in that same pass, so an extra presentation
  cannot draw any of that text twice.

  The D3D8 overlay can show the rate the frames are actually presented at as well now, next to the
  render time line in the bottom left corner, and the D3D8 hook is what counts the presentations.
  That is what the D3D9 overlay has shown for th10 to th20, and th08 is the first D3D8 game to get it.

  Checked in game at 180Hz: three presentations per frame 5.56ms apart, 180.6 presentations per
  second, the game's own frame rate reading 60 with its own text at "60.00fps", the bullets, the
  player, its shots, the familiars (all five teams) and the straight lasers all smooth with no jumps,
  ghosting or misalignment, and thprac's input latency readout back where it was before the extras
  were added.

  Not done, as the requirements say: the enemies (their animation scripts place them, there is no
  velocity to read) and the curve lasers.

## 2026-10-14

### Added

- **Touhou 8 gets the input handling vpatch applies to it** (`dd75d02`, `2d18dbd`)

  Imperishable Night's player input does not come from the keyboard directly: `Player::HandlePlayerInputs`
  (`0x44AEC0`) and `Player::__shoot_bullets` (`0x451500`) read the copy at `0x164D52C`, and the game
  only refreshes that copy in `ReplayManager::OnUpdateHighPrioRecord` (`0x452310`) - the calc chain
  element at priority 17, after the input has been polled (priority 0) and after the player has run
  (priority 9). Every frame therefore used the previous frame's input, which is the one frame of input
  lag vpatch's readme describes for this game (`永夜抄は、妖々夢と同じ入力遅延が1フレームあります`) and
  the one its rev4 changelog says it removed.

  vpatch's four patches are what removes it: the
  call to `Controller::GetInput` inside `Supervisor::OnUpdate` (`0x4455F0`) goes to a wrapper that
  advances the copy at the moment the input is read (pause bit filtered, and left alone while a replay
  is playing), the copy work inside the record update (`0x452326`) is skipped, the flag that update
  tests (`0x452319`) is spent there, and the return of `ReplayManager::AddedCallbackRecord`
  (`0x452D52`) raises that flag so a stage's first frame does not carry its selection keys over. The
  play test is what says it worked: the input lag this game had been chased for since the first round
  is gone, and it was the answer to "why is oilp a frame behind vpatch here" all along.

  The same copy handling is also what vpatch's own ini documents its `AlwaysBlt` for this game with -
  keys staying held after the window loses focus are its other effect.

- **Touhou 7 gets its own version of the same input handling** (`2d18dbd`)

  Perfect Cherry Blossom's player reads `g_CurFrameGameInput` (`0x4B9E50`, with `0x4B9E58` as its
  previous frame) - a copy the game only advances in `ReplayManager::OnUpdateRecord` (`0x442CD0`), the
  calc chain element at priority 16, after the player has run (priority 8). That is the extra frame of
  input lag vpatch's readme lists for this game next to the drawing one, and what upstream's "run the
  drawing logic after the game update logic" never touched. Here the call to `Controller::GetInput`
  (`0x437D80`) starts going through a wrapper that advances the copy at the moment the input is read
  (left alone while a replay is playing), the game's own copy job is taken over (`0x442CD9`) with its
  copy pair skipped (`0x442CF2`), and `ReplayManager::AddedCallbackRecord`'s return (`0x4433A5`)
  raises the same flag Touhou 8's does.

  Both games' handling is applied the way vpatch applies its own: unconditionally. (The first version
  of this had it behind `FixInputGlitching`; that gate is gone.)

### Changed

- **`FixInputGlitching` is on by default** (`dd75d02`)

  What it covers is the keyboard state EoSD and PCB read while the window is not in the foreground:
  DirectInput refuses to hand the state over (`DIERR_NOTACQUIRED`), the game only treats
  `DIERR_INPUTLOST` as a failure and goes on reading a buffer that was never filled in, which shows up
  as keys being pressed out of nowhere. `AlwaysBlt`, on by default as well and only about keeping the
  game running, is what walks into that every frame - in vanilla the whole render bails out while
  unfocused, so this could only be reached with that combination of options. The input timing fixes
  the two entries above are about are not this option, on either game.

### Fixed

- **D3D8 games are no longer told to use a discard swap effect** (`dfdcaf5`)

  `apply_presentation_overrides` was forcing `D3DSWAPEFFECT_DISCARD` and a back buffer count of 0 on
  every D3D8 game, and those games read back buffer 0 right after presenting: `GameWindow::Present`
  (`main.cpp:368`) calls `Present()` and then `AnmManager::TakeScreencaptures`, which does
  `GetBackBuffer(0, D3DBACKBUFFER_TYPE_MONO)` and copies what it finds into the surfaces the afterimage
  effects (and the pause screen's background) are drawn from. DISCARD means exactly "the contents are
  undefined after a present", so those captures stopped reading the frame that had just been drawn. On
  Touhou 8 that was the afterimage trail in the stage 6A boss intro looking different, and thprac's
  overlay over the same frames disappearing a few frames late; both look right with the two settings
  left to the game. The refresh rate choice (`FullscreenRefreshRate` still applies to the D3D8 games)
  and vsync being forced off stay.

Not done yet: the keyboard device is still only acquired once at startup, so if keys can still be left
held after losing focus once the copy handling is in, an `Acquire()` on activation is the next step
(vpatch does not do that either - its own answer is a background cooperative level, which is left
alone here). Touhou 8's frame interpolation is still open. The D3D9 hook still forces the same discard
swap effect on the D3D9 games; that is left alone until a game shows a reason.

## 2026-10-13

### Added

- **Touhou 19 (Unfinished Dream of All Living Ghost) gets frame interpolation too** (`3262ede`, `4ac58a3`, `3203d04`, `8d5a809`, `3f551be`)

  Unfinished Dream of All Living Ghost is two things the earlier games are not: it is a **versus
  game** (each side has a set of managers of its own, so the bullets, the lasers, the players and
  their shots all had to be done twice) and it is the **first build with ASLR** (the executable is
  relocated, so every address from the disassembly has to be rebased onto where it actually loaded
  before it is used). It has no replay system, so there is no replay speed control, and its frame
  pacing is its own: an update only moves the engine's own clock along by one frame's worth and the
  game's logic (and rendering) only runs once that clock says the next frame is due, with the
  length of a frame coming from a hardcoded 60fps in a few instructions - so replacing the game's
  own frame limiter is not enough for `GameFPS` to do anything, and those instructions are pointed
  at the patch's frame rate as well (the same thing thprac's FPS option does for th11 and up).

  The extra presentations: four render blocks (`0x4B5E32`, `0x4B6189`, `0x4B65E5`, `0x4B76EB` - the
  last two are in dead code and are taken over anyway) and five presents (`0x4B5EFF`, `0x4B61C4`,
  `0x4B5D0C`, `0x4B5F86`, `0x4B642E`), each a 6 byte device load plus 14 bytes of arguments with
  all five of them inside the part that is skipped, the same shape th18's have. The device is
  `[0x62BB48]`, the sprite (ANM) manager `[0x5D19BC]`, the task manager `[0x5D1A24]`, the dword the
  block sets to `0xFF` is `[0x62C70C]`, and the byte that says how many of the game's updates a
  rendered frame covers is `[0x62BDDC]` (read and never written, so it is always 0). There is no
  camera and no SetTransform anywhere in the frame functions, so an extra presentation restores the
  viewport and the transforms it read back from the device. Two problems th19 has of its own were
  fixed along with it: the overlay's "X ms" readout was too high by exactly the interpolation
  factor (the limiter's TickUntil measured the whole wall clock frame with the extras included in
  it, and now looks at frame_work_end the way Tick does), and a frame rate pushed to 500-600
  through thprac could not be brought back down (with a frame of 1.67-2ms the game's own pacing
  branch is rarely taken, so TickUntil is never called with it and game_fps is never read again;
  the present hook now polls the external frame rate every frame through
  Limiter::PollExternalGameFPS).

  The bullets are one manager per side at `[0x5D1A60 + side*0x3C]` (a pool of 2001 of `0x1088`
  bytes): what is in play is on the six layer chains the update walk rebuilds every frame (heads at
  `manager+0x14+i*4`, tails at `+0x2C+i*4`, the next bullet at `bullet+0x6B8`, the layer number at
  `+0x6C0`). The position is `+0x650`, the velocity `+0x65C` (with the angle at `+0x670` and the
  speed at `+0x668`), the 0x200 flag on `+0x10` holds a bullet, the state is `+0x1044`, the shared
  slowdown is `[0x5C6418]`, and the picture being held is `[0x5D1A54]+0x140` (the update looks at
  `((f>>2)|f)&1` and `f&0x800`, the drawing at `f&4`). What is drawn, though, is the bullet's own
  `+0x62C` - the draw walk copies `+0x650` into it, and the extras do replay the draw walk - so
  moving the position is all it takes.

  The player is a `PlayerInf` (one per side, `0x14C48` bytes, at `[0x5D1A60+side*0x3C+4]`) with its
  position at `+0x68C` (three floats). It has no velocity to read - it moves by the input the game
  processed that frame, in per character code - so it is measured the way the games after th13
  measure theirs: the step the game's own frame made is kept, and an extra presentation draws it
  that step's t further along and puts it back right after the render. The player's own shots are
  not on the bullet manager's chains and the game's class list has no shot class for them, so they
  were found by what they look like: a pool of 16 slots the player owns (from `player+0x22F8`,
  `0x128` bytes each, the sprite id at `+0x14` and the position at `+0x54`). They are measured the
  same way, and both the slot's position and the sprite's copy of it (at `+0x614`) are moved - the
  picture for a shot is drawn from the copy the game's own update bakes into the sprite, and that
  update is not replayed.

  The straight lasers are on the side object at `side_record+0x14` (a `LaserInf`): the newest node
  of the list of lasers in play is at `manager+0x14`, a node sits inside the object it holds at
  `+8` (the object at `node+0`, the one before it at `node+4`), and how many there are is the
  manager's `+0x38`. The position (`+0x60`), the angle (`+0x78`) and the length (`+0x7C`) are read
  where the picture is drawn, so writing them is all an extra presentation has to do. Only the two
  straight kinds are projected, by vtable: `LaserLineInf` (`0x5A885C`) and `LaserInfiniteInf`
  (`0x5A873C`) - and the two do not even share the fields they are moved by (the line one grows its
  length to its own limit at `+0x898` first and only then moves, by its velocity at `+0x6C..+0x74`;
  the infinite one recomputes its position out of another object's `+0x888` and adds
  `+0x894..+0x89C` to it, turning by the rate at `+0x8A4`), so these are measured as well: the
  position, the angle and the length are measured every game frame, an extra presentation writes
  them that step's t further along, and a laser is identified by its address together with the id
  its manager gave it (`+0x94`), so that a new laser at a reused address projects nothing. Beams
  (whose update is a stub) and curves are left alone.

  The menus' text is the same trap th12 to th18 have, with fields of its own: the text manager is
  `[0x5D1A34]` (an `AsciiInf`), the records are 0x13C bytes each from `+0xC78` (0x140 of them at
  most), and how many of them the frame has is `+0x19778` (the append is FUN_004E6C30; the aging
  that drops the records whose life at `+0x130` ran out, compacts the rest and writes the surviving
  count back is FUN_004E6F00 through FUN_004E5750, registered with the update chain; the drawing is
  six per layer tasks on the draw chain calling FUN_004E6B60). The render hook keeps that count and
  every extra presentation puts it back before it renders, so each one appends its records in the
  same slots the game's own pass used and draws the same text it drew.

  What the test said: on a 180hz display the steady state is `3 present(s)` a frame, 5.56ms apart,
  and the stage test averaged `2.983 presents per frame, 179.0 presentations per second`; the menu
  test, which is where the text was checked, averaged 2.847. The frames that come out below 3 are
  the ones where an extra presentation would not have finished before the next game frame was due
  and was dropped, which is the patch's own rule from th17 and th18 rather than anything new. The
  bullets were `125 in play, 125 projected`; the player `2 of 2 moved` at 3.44px a frame; the shots
  `7 in play`; the straight lasers `8 in play, 8 projected` with the managers' own counters saying
  8 as well, two independent numbers agreeing; `0 restore(s) wrong`, so no extra presentation ever
  left a position behind for the game's own logic to walk on top of; and the text list counted
  `0 -> 2 -> 2` records (before the game's own pass of the walk, after it, and at the end of the
  frame), the last two being equal, which is what says the same text is not drawn twice any more.
  Both the picture and the text were confirmed on screen.

  Two known approximations are left: the endless laser (`LaserInfiniteInf`) never showed up in the
  tests, so its branch is verified from the code only (its drawing reads the same position and
  angle the line one does, and it goes through the same measuring), and the shots and the lasers
  both extrapolate from the previous frame's step, which is first order for anything that changes
  speed within a frame (a laser in its first frame, or one changing phase).

- **Touhou 20 (Fossilized Wonders) gets frame interpolation too** (`a966e0c`, `7d792cd`, `fb2771f`, `e948913`, `5f5ab65`)

  Fossilized Wonders runs on th19's engine - the one that keeps a clock of its own, and whose frame
  length comes from a hardcoded 60fps read by a handful of instructions - so the base support is the
  same shape. What it is not is th19: this is a single player game (only one record of the side table
  is in use), and none of th19's offsets carried over - every one of them was read out of th20
  itself, with th19 used only for what the pieces should look like.

  Extra presentations: three render blocks (0x419D3B, 0x419F91, 0x41A10C) and three presents
  (0x419666, 0x4199B2, 0x419B2E, each a 5 byte device load followed by 34 bytes of arguments that
  are all skipped, since all five of them are pushed inside). The device is [0x5C4D48], the sprite
  (animation) manager [0x5C0028], the scene/task manager [0x5B66D8], the dword the render block
  raises [0x5C5AF8] and the frames-per-render byte [0x5C4F84]. The frame functions set no transform
  either, so an extra presentation puts the viewport and the two transforms back from the device.
  Also here: `Limiter::PollExternalGameFPS()` once per frame (without it thprac pushing the logic
  frame rate up can't be brought back down here either), and a wrong comment about the frame limiter
  being ticked from the EndScene hook.

  Bullets: the manager is `*(0x5BA568 + n * 0x30)` (the side record's +0; a pool of 2000 bullets of
  0x528 bytes), and the ones in play are on the six layer chains the update walk rebuilds every
  frame (heads at +0x14 + i*4, tails at +0x2C + i*4, threaded through bullet+0x60, layer at +0x44).
  A bullet keeps its position at +0x64 and its velocity at +0x70, its state at +0x50, is held by
  0x200 of +0x14, and moves by `position += velocity * the global at [0x5AEFE4]` in state 1 and by
  `position += velocity / 2` (the half step) in states 2 and 3; the scale it moves by is its own
  two factors, +0x4D8 * +0x4E8, copied into that global before it moves. The drawing reads the
  bullet's own position (the draw walk copies +0x64 into the sprite at +0x5BC), and that walk is
  what the extras replay, so moving the position is enough.

  The player: `*(0x5BA568 + 4)` (a PlayerInf whose fields run past +0x14854), with its position at
  +0x614 - the same point is kept twice more, at +0x5E4 and +0x694, 32 pixels apart in y, and all
  three are moved. There is no velocity to read, so it is measured, the way the games after th13
  measure their players. Its own shots are a pool of 256 slots of 0x124 bytes at player+0x22B4
  (position at +0x50, sprite id at +0x18), measured the same way; and because the game bakes a
  shot's position into its sprite while it updates it - which the extras don't replay - the sprite's
  drawn position at +0x5BC is written as well. That sprite is found by reproducing the array branch
  of the game's own lookup: sprite = [0x5C0028] + 0x710 + (id & 0xFFFF) * 0x600, with the in use byte
  at +0x5F8 and the id at +0x4C0.

  Straight lasers: the manager is the side record's +0x14, its newest list node is at manager+0x14,
  a node sits inside the object at +8 (object at +0, next at +4) and the count is at +0x28. Both of
  the two straight kinds are drawn from fields read where the picture is drawn - position +0x58,
  angle +0x70, length +0x74 - and the whitelist is by vtable (LaserLineInf 0x571148,
  LaserInfiniteInf 0x5712E8), since the two kinds do not even share the fields they are moved by,
  which is why they are measured as well (told apart by the object pointer and the id at +0x8C). The
  beam kind's update is a stub and the curve one is not a straight line, as on the other games.

  The menus' text: the manager is an AsciiInf (constructor FUN_0046C080), its records are a flat
  array at +0x11BC (0x140 bytes each, up to 0x140 of them), the count of the ones to draw this frame
  is at +0x1A1BC and a record's life is at record+0x134 (aged by the update task with id 8). Unlike
  th19 the manager is not in a global at all - the loading thread builds it and only tests the
  result - so it is caught where it is constructed (the call at 0x471084). The count is put back
  before every extra presentation, so the extra presentations append in the same place the game's
  own pass did instead of on top of its records.

  In game (180Hz, Interpolation = -1): 3 presentations per frame in the steady state, 5.56ms apart;
  478 of 478 bullets projected, against a manager whose own counter says 478; the player moving
  4.50px a frame; 118 of 118 shots projected (108 of them through their sprite); 154 of 154 straight
  lasers projected, with the managers' own counters saying 154 - the walk and the game's own
  bookkeeping agreeing, which is what says the chains are followed the way the game follows them;
  and the text list coming out at the same count on both sides of the extra presentations (0 -> 50 ->
  50), which is what says the same text is no longer drawn twice. The picture and the feel were
  confirmed by the user.

  Known approximations: the endless laser kind never appeared in the tests (it was only checked in
  the code), and the shots and the lasers extrapolate from the previous frame's step, which is first
  order for anything that changes speed within a frame.

### Changed

- **The shipped config and the built-in default for `Sleep` go back to `1` (the vpatch wait)** (`eb6f481`)

## 2026-10-12

### Added

- **Touhou 18 (Unconnected Marketeers) gets frame interpolation too** (`3e0ad73`, `d73f0df`, `ab192a5`, `f19deb1`, `7be70e3`)

  Unconnected Marketeers is the same family as Wily Beast and Weakest Creature but **not the same
  generation**: the idea of three render blocks and three presents is the same, what is inside the
  block is not, so every address was looked up in th18 itself. The device is `[0x4CCDF8]`, the
  sprite (ANM) manager `[0x51F65C]`, the dword the block sets to `0xFF` is `[0x4CD918]`, the queue
  is reset by `0x47E6E0` and drawn by `0x47E730`, and the objects are rendered by `0x401420`; the
  renders are at `0x471BCB`, `0x472F21` and `0x47307A` and the presents at `0x472D1D`, `0x472F5C`
  and `0x473129` (the same 5 byte device load and 14 bytes of arguments, with all five arguments
  inside the skipped part and nothing in there writing a register the code after it needs).
  **Three things are laid out differently from th17**: there is no "prepare the render" call (the
  block sets D3DRS_FOGENABLE to 0 itself), the sprite queue is drawn twice per block (once right
  after the reset and once after the objects are rendered), and there is no camera object and no
  SetTransform anywhere in the frame functions - the frame hands the device the viewport at
  `[0x4CD5DC]+0x10C` and copies two integers (`+0x104/+0x108`) into the sprite manager as the
  floats at `+0xD8/+0xDC`. An extra presentation therefore restores the viewport and the
  transforms read back from the device, and puts those two floats back as well.

  The bullets are the manager `[0x4CF2BC]` (one 0x7A41F8 byte object): a pool of 2001 bullets of
  `0xFA0` bytes from `manager+0xEC` (`0xEC + 2001 * 0xFA0` is `0x7A228C`, exactly where the
  constructor starts clearing the 2001 words that follow it), threaded onto a list whose first node
  is at `manager+0xC0` and whose node is part of the bullet at `+0x10` (the bullet at `node[0]`,
  the next node at `node[4]` - both the game's own update walk at `0x424C50` and the routine that
  creates one, `FUN_00424FE0`, agree). The position is `+0x638`, the velocity `+0x644`, the 16 bit
  state `+0xF68` (1 moves a whole step, 2 and 3 half of one), the 0x200 flag on `+0x20` holds a
  bullet, the scale is `[0x4CCBF0]`, the half step is the 0.5f at `[0x4B9138]`, and the byte that
  says how many of the game's updates a rendered frame covers is `[0x4CD00C]` (read and never
  written, so it stays 0). **What holds the picture is a different object this time**: the game
  thread at `[0x4CF2E4]`, its flags at `+0xB0` (the update task stops for the low bits, the draw
  task for bit 2, the walk itself for bit 0x400). th18 has one more of those - with the ability
  shop open (`[0x4CF2A4]` set) the bullets stop as well - and that counts as a held picture too.

  The player is `[0x4CF410]` (0x479D0 bytes): its own position at `+0x620` (the render pass
  `0x45CAC0` copies it into the animation object at `+0x14`'s `+0x5F0` while drawing), the shots
  are **128 slots of `0xF8` bytes at `+0x1570`** with the same layout th17's had (`+0x8C` in use,
  the sprite id at `+0x08`, the position at `+0x48`, which the slot's update copies into the sprite
  at `+0x5F0`), and the sprite the player's own shot is drawn with is kept at `+0x476B0`. **The
  ability cards** the user called out work out like this: every card keeps the animation id of its
  in-game effect at `+0x1C` (thprac's `anm_id_for_ingame_effect`; both `0x41051E` and `0x410AD6`
  push it into the sprite lookup), and the card list hangs off the ability manager
  (`[0x4CF298]`) at `+0x1C`, with the card at `node[0]` and the next node at `node[4]` (the walk
  the player's own update does at `0x45C0D1`) - so those sprites are projected with the player's
  own. What a card adds to the player's fire is a shot like any other and lands in the same slot
  array. Sprites are found from an id the way the game finds them, written out rather than called
  (slot = `[0x51F65C] + 0x700 + (id & 0x7FFF) * 0x624`, in use at `+0x61C`, id at `+0x544`), and
  their draw position is `+0x5F0`.

  The straight lasers are the manager `[0x4CF3F4]` (one 0x7C4 byte object, built right after the
  bullet manager): the list starts at `manager+0x14`, the next is at `obj+0x08` and `obj+0x10 == 1`
  means the laser is finished (not drawn, and not projected). The geometry is the same th15 to
  th17 have - the position at `+0x54`, the angle at `+0x6C` and the length at `+0x70`. The
  velocity reading the other games use carries over: the vtables come out of the RTTI in two steps
  (Line `0x4B687C`, Infinite `0x4B680C`, Beam `0x4B679C`, Curve `0x4B672C`), and the whitelist
  holds only Line (velocity `+0x60`, length rate `+0x78`, clamp `+0x798`, no turning) and Infinite
  (velocity `+0x794`, angle rate `+0x7A4`, length rate `+0x78`, clamp `+0x7A8`). The beam class's
  update (`0x452B80`) is a `return 0` stub, so it never moves and is left out; the curve class is
  left out as the requirements ask.

  The menus' text got its fix at the same time, including the shop: the text object is `[0x4CF2AC]`,
  the record array is at `+0xC24` (0x138 bytes a record, up to 0x140 of them), the frame's count
  of them is `+0x19224` (appended by `0x419B90`, printed through `0x41A110`, walked by `0x41A180`;
  it is reset every frame by the aging task `0x4198B0`, which sits on the update chain), and the
  render hook keeps the count for every extra presentation to put back.

  In game the extra presentations land evenly at 180 a second (`average: 3.000 presents per frame,
  180.6 presentations per second`), the diagnostics read `659 in play, 659 projected` bullets, `99
  sprite(s) moved` for the player, its shots and its cards' effects (with the count of sprites that
  had no movement record staying 0), and `24 in play` straight lasers, and the title, the menus,
  the pause screen, the shop and the replay saving screen all look the way they do without the
  patch.

- **Touhou 17 (Wily Beast and Weakest Creature) gets frame interpolation too** (`a2f749b`, `13b2f6b`, `a4ce5b3`, `74d1665`)

  Wily Beast and Weakest Creature is Hidden Star in Four Seasons' engine again, so the whole path
  follows th16's - with every offset looked up in th17 itself rather than carried over. Its frame
  functions have the same **three render blocks and three presents** (the renders at `0x461248`,
  `0x4622C4` and `0x4623FD`, the presents at `0x4620FD`, `0x4622FF` and `0x4624AC`, the second one
  sitting inside the window update, so the title screen and the menus get the extra frames as
  well). The device is `[0x4B5AE8]`, the scene object `0x4B5AE0`, the sprite manager `[0x509A20]`,
  the dword the block sets to `0xFF` is `[0x4B64F0]`, the queue is reset by `0x46D050`, prepared by
  `0x4421C0`, filled by the render at `0x4013E0` and drawn by `0x46D0A0`. **The camera is the one
  thing laid out differently from th16**: there is no function that takes the camera object - the
  frame function calls `0x409460` with the scene object and the index of the view it wants
  (`push 2; mov ecx, 0x4B5AE0`), and that picks one of the views the scene keeps (the one in use
  is pointed at by `[0x4B5AE0+0x6E8]`, each view 0x12C bytes: the view transform at `+0x60`, the
  projection at `+0xA0`, the two queue placement floats at `+0xFC/+0x100` and the viewport at
  `+0xE0`). An extra presentation reads that pointer, so it never has to guess an index. All three
  presents are laid out identically (5 bytes of device load then 14 bytes of arguments, with all
  five arguments inside the skipped part).

  The bullets are the manager `[0x4B768C]`: one pool of 2001 bullets of `0xE88` bytes (from
  `manager+0xEC`), with the bullets in play on a list whose first node is at `manager+0xC0` and
  whose node is part of the bullet itself (bullet`+0x10`, the bullet at `node[0]` and the next node
  at `node[4]`). The position is `+0x62C`, the velocity `+0x638`, the 16 bit state `+0xE50` (1
  moves a whole step, 2 and 3 half of one), the 0x200 flag on `+0x20` holds a bullet, what
  everything is scaled by is `[0x4B5918]`, and `[0x4A3B48]` is the 0.5 the half steps use. The byte
  that says how many of the game's updates one rendered frame covers (`[0x4B5CD4]`) is only ever
  read in this executable and always 0. One mechanism is th17's own: the movement all lands in the
  generic `pos += velocity * [0x4B5918]` at `0x415121`, while the state 1 script interpreter
  (`0x4165B0`) and the mode handlers only ever change the velocity and the angle - so projecting
  from the velocity still holds.

  The player is `[0x4B77D0]` (0x190FC bytes): its own position is `+0x610` (the render pass at
  `0x448CF0` copies it into the animation object it carries, `+0x5F4`, while it draws). **The beast
  spirits (the "goast" - what sits beside the player and what the roar summons) are not slots of
  the player in this game at all**: they live in a manager of their own at `[0x4B7684]` (built with
  `beast.anm`), each one a 0x70 byte object on its list (the first at `[manager+0x18]`, the next at
  `+0x04`), keeping the sprite id it is drawn with at `+0x10`, **the sprites the roar's aura and
  effects are drawn with at `+0x28`/`+0x2C`/`+0x30`/`+0x38`/`+0x44`** and its position at `+0x18`
  (its update `FUN_00410AF0` writes that into the main sprite every frame). The shots are 256 slots
  of 0xE0 bytes at `+0x1110` (`+0x8C` in use, the sprite id at `+0x08`, the position at `+0x48`),
  and the sprite the player's own shot is drawn with is kept at `+0x19094`. The sprites are
  resolved from their ids **every frame**, with the array branch written out rather than called
  (the manager at `[0x509A20]`, slots of 0x618 bytes from `+0x6EC`, the "in use" byte at `+0x610`
  and the id at `+0x538`, since the game's own lookup `0x476470` takes its argument on the stack),
  and what gets moved is the sprite's position at **`+0x5E4`**. Like th15 and th16, these three are
  measured rather than read.

  The hyper the game is known for is a feature of that same beast manager (thprac hooks the roar at
  `0x40FC8A`, inside the 0x40Fxxx block that manager's code lives in), so the effects it draws are
  those sprite ids of the beasts and the special shots it fires go through the same shot slots -
  both come along with the player for free.

  The lasers are the manager `[0x4B76BC]`: the list starts at `manager+0x14` and its links are the
  object's own `+0x08`, and the geometry is th15's and th16's exactly (position `+0x54`, angle
  `+0x6C`, length `+0x70`; the line laser's draw at `0x437A40` writes them into the animation
  objects it carries while it draws). They use the **read-the-velocity** way of doing it: the
  vtables come out of the RTTI (LaserLineInf `0x4A1924`, LaserInfiniteInf `0x4A18B8`, LaserBeamInf
  `0x4A184C`, LaserCurveInf `0x4A17E0`), with one twist this game has - the `.*AVLaser*Inf@@` name
  strings are not in `.rdata`, and the first hit `find:` gives for one of them is not the locator
  but a second table that also names the type. The hit that matters is the one inside an
  RTTICompleteObjectLocator, so the locator is that hit minus 0xC, and `find:` of the locator gives
  `vtable - 4`. The whitelist holds only the line class (velocity `+0x60`, rate `+0x78`, limit
  `+0x5E4`, and it doesn't turn) and the infinite one (velocity `+0x5E0`, angle speed `+0x5F0`,
  rate `+0x78`, limit `+0x5F4`); the beam class needs no entry because its update `0x440520` is a
  stub that returns 0, and the curve class is left alone.

  The menus' text (the bug th12 to th16 had) is handled in the same round: the text object is
  `[0x4B7678]`, its records are 0x138 bytes each from `+0xC10` (up to 0x140 of them), and the count
  of them for the frame is `+0x19210` (appended by `FUN_00408190`, walked by `FUN_00408560`). The
  render hook keeps that count and puts it back before every extra presentation.

  Measured: on an 180Hz display the extra frames land evenly at 180 a second (`average: 3.000
  presents per frame, 180.6 presentations per second`), with `161 in play, 161 projected` bullets,
  `42 sprite(s) moved` for the player, its beast spirits and its shots with none of them missing a
  movement record, and `86 straight laser(s) in play`. The title, the menus, the pause screen and
  the save screen all look right, with the replay information line on the save screen showing up
  once.

## 2026-10-11

### Added

- **Touhou 16 (Hidden Star in Four Seasons) gets frame interpolation too** (`9c0cb56`, `22d89ad`, `0ab4c41`, `1e673bb`)

  Hidden Star in Four Seasons is the same engine as Legacy of Lunatic Kingdom, so the whole path
  follows th15's - with every offset looked up in th16 itself rather than carried over. Its frame
  functions have the same **three render blocks and three presents** (the renders at `0x45A99C`,
  `0x45AD9C` and `0x45AF28`, the presents at `0x45ABAA`, `0x45ADF6` and `0x45AFFB`, one of which
  sits inside the window update `CWindowManager__UpdateFast` at `0x45AC50`, so the title screen and
  the menus get the extra frames as well). The device is `[0x4C10D8]`, the render context
  `0x4C10D0`, the sprite manager `[0x4C0F48]`, the dword the block sets to `0xFF` is `[0x4C1ADC]`,
  the queue is reset by `0x465A30`, prepared by `0x43C4F0`, filled by the render at `0x4015A0` and
  drawn by `0x465A80`; the camera is `0x43C710(0x4C1560)` (it hands the device the view and
  projection transforms and the sprite manager the two values that place its queue on screen) and
  the viewport is at `0x4C1640`. All three presents are laid out identically - 5 bytes of device
  load then 14 bytes of arguments - so one hook covers them, with none of the instruction-in-the-
  middle-of-a-setup trouble th13's second present had.

  The bullets are the manager `[0x4A6DAC]`: two pools of 2001 bullets of `0x1478` bytes (the first
  at `+0x9C`, the second at `+0x9FFE94` being the copy the game keeps for its replay snapshots),
  with the bullets in play on a list whose first node is at `manager+0x70` and whose node is part
  of the bullet itself (bullet`+0x10`, the bullet at `node[0]` and the next node at `node[4]`).
  The position is `+0xC20`, the velocity `+0xC2C`, the 16 bit state `+0xC72` (1 moves a whole step,
  2, 3 and 5 half of one), the 0x200 flag on `+0x20` holds a bullet, and what everything is scaled
  by is `[0x4A5788]` (with `[0x49449C]` being the 0.5 the half steps use). The byte that says how
  many of the game's updates one rendered frame covers (`[0x4C12C4]`) is only ever read in this
  executable and always 0, so one rendered frame is one update. One detail is th16's own: a bullet
  in state 5 doesn't move until its timer at `+0x1450` is past 2, and the projection reproduces
  that instead of treating it as noise.

  The player is `[0x4A6EF8]` (0x2C828 bytes): its own position is `+0x610` (the render pass at
  `0x443730` copies it into the animation object the player carries). **The options are two arrays
  of slots**, 4 at `+0x660` and 8 at `+0x9F0`, 0xE4 bytes each (`+0x00` in use, the target at
  `+0x54/+0x58`, the position in 1/128 pixels at `+0x5C/+0x60`, two sprite ids at `+0xB0/+0xB4`);
  the shots are 256 slots of 0xC0 bytes at `+0x1118` (`+0x84` in use, the sprite id at `+0x00`,
  the position at `+0x40`). The sprites are resolved from their ids **every frame** (the array
  branch written out rather than called: the manager at `[0x4C0F48]`, slots of 0x614 bytes from
  `+0xEC`, the "in use" byte at `+0x60C` and the id at `+0x538`, since the game's own lookup
  `0x46EFA0` takes its argument on the stack), and what gets moved is the sprite's position at
  **`+0x5E0`** (th16's animation objects sit 0xC lower than th15's). Like th15, these three are
  measured rather than read: the player moves by what the input says, the options walk toward it
  and the shots add their own motion on top, so there is no velocity to read back.

  The lasers are the manager `[0x4A6EE0]`: the list starts at `manager+0x14` and its links are the
  object's own `+0x08` and `+0x04`, and the geometry is th15's exactly (position `+0x54`, angle
  `+0x6C`, length `+0x70`; the line laser's draw at `0x433720` writes them into the two animation
  objects it carries while it draws). They use the **read-the-velocity** way of doing it, which
  th12 to th15 were changed to in the same round: four vtables come out of the RTTI (LaserCurveInf
  `0x4922E0`, LaserBeamInf `0x49234C`, LaserInfiniteInf `0x4923B8`, LaserLineInf `0x492424`), the
  whitelist holds only the line class (velocity `+0x60`, rate `+0x78`, limit `+0x5E4`, and it
  doesn't turn) and the infinite one (velocity `+0x5E0`, angle speed `+0x5F0`, rate `+0x78`, limit
  `+0x5F4`), the beam class needs no entry at all because its update `0x43AC00` is a stub that
  returns 0, and the curve class is left alone.

  The menus' text (the bug th12 to th15 had) is handled in the same round: the text object is
  `[0x4A6D98]`, its records are 0x138 bytes each from `+0xBF0` (up to 0x140 of them), and the count
  of them for the frame is `+0x19208` (appended by `FUN_00408140`, walked by `FUN_00408560`). The
  render hook keeps that count and puts it back before every extra presentation. th16 doesn't need
  the twelve layout fields th13 did: its record draw (`FUN_00408650`) writes none of them.

  Measured: on an 180Hz display the extra frames land evenly at 180 a second (`average: 3.000
  presents per frame, 180.6 presentations per second`), with `622 in play, 622 projected` bullets
  (and `114 in play, 114 projected` in another run), `98 sprite(s) moved` for the player, its
  options and its shots (with none of them ever missing a movement record) and `55 in play, 55
  projected` lasers; the title screen, the menus, the pause screen and the replay save screen are
  all normal.

- **Touhou 10 (Mountain of Faith) gets frame interpolation too** (`229342c`, `7036f2d`, `dcd812a`, `cf6f78d`, `b9c8811`)

  Mountain of Faith is the oldest of this batch (2007, a generation before Subterranean Animism),
  and it draws a frame the way th11 does with less of it: **one render block and one present**, both
  inside the frame function `CWindowManager__Update` (`0x439390`) - the render block calls the
  object render at `0x4394D8` and presents at `0x43920A` - so there are two addresses to take over
  rather than six, and since that one render block sits in the window update, the title screen and
  the menus get the extra frames as well. The device is `[0x491C30]`, the render context
  `[0x491FAC]`, the sprite queue's owner `[0x491C10]`, the byte the block sets to `0xFF` is
  `0x492378`; the object render is `FUN_00449D40` (its object in `EDI`) and the queue is drawn by
  `FUN_00442F50` (its object in `ESI`), so both go through the same `__declspec(naked)` trampolines
  th13 needed. Its present is laid out in pieces - the three pushes in front of the patched load
  belong to the same call - so the hook has to return with `ret 0xC` and put those 12 bytes of
  arguments back; getting that wrong is what made th10 fail its stack cookie and die on startup the
  first time it was tried.

  The bullets are the manager `[0x4776F0]`, whose pool is 2001 bullets of `0x7F0` bytes at
  `manager+0x60` and whose six column lists (heads at `manager+0x14`, next at `bullet+0x44C`) are
  both what the update rebuilds and what the draw walks, so they are exactly the bullets that are
  drawn. Position at `+0x3B4/+0x3B8/+0x3BC`, velocity at `+0x3C0/+0x3C4/+0x3C8`, state at `+0x446`
  (1 is a full step, 2 and 3 are half of one), global slowdown at `[0x476F78]`, and the drawn
  position the draw copies the position into at `+0x33C/+0x340/+0x344`. The update divisor
  `[0x491D66]` is read in the executable and written nowhere and is 0, so there is exactly one
  update per rendered frame.

  The player is `[0x477834]` (0x4478 bytes). Its update (`FUN_004250B0`) moves it as two ints in
  1/128 pixels at `+0x3CC/+0x3D0` and writes the two floats at `+0x3C0/+0x3C4` back out from them,
  and its draw callback (`FUN_00426360`) copies those into the animation object it carries at
  `+0x354/+0x358` every frame, so moving the logical position is what moves the body - th13's and
  th15's rule. Its options are 4 slots of `0x98` bytes at `+0x32A0` (in use when `+0x00` is
  non-zero, position at `+0x34/+0x38` as ints in 1/128 pixels, two animation objects at
  `+0x68/+0x6C`) and its shots are a pool of 128 slots of `0x5C` bytes at `+0x49C` (in use when
  `+0x40` is non-zero, position at `+0x14/+0x18/+0x1C`, two animation objects at `+0x44/+0x48`).
  The shots are filled in by `FUN_00427E90`, walked every frame by `FUN_00428280` and tested against
  the enemies by `FUN_00428630`, and all three of those work on that one pool, which is what says
  its shape is right (the offsets are each read out of those functions rather than carried over
  from th11, whose pool is at `+0x96C` with 0x6C byte slots). Both the options and the shots are
  positioned by the update rather than by a draw callback, so the objects are what is moved for
  them, and they are looked up by handle again every frame (walking the sprite manager's two lists
  at `[0x491C10]+0x72DAD4`/`+0x72DADC`; an object's drawn position is `+0x340/+0x344/+0x348`),
  which is what keeps the crash th12 had - a cached pointer to a freed sprite - from happening.

  The stage also builds **one single shot object** of its own (`FUN_004056B0`, 0x48 bytes, kept at
  `[0x4776EC]`), updated by `FUN_00405750` and fired by `FUN_00405860` from the player's own shot
  state machine (`FUN_00425730`). It is projected as well, so that a shot of that one's isn't left
  stepping while the pool's are smooth, but in the runs below it was never in flight: the pool is
  what the player's fire actually shows.

  The straight lasers are the manager `[0x47781C]` (one 0x45C byte block, the node the game works
  from at `+0x10`; list at `manager+0x18`, next at `laser+0x08`, state at `+0x0C`). th10 carries
  **no RTTI at all** - its only RTTI names are the eight MSVC standard library ones - so the classes
  are whitelisted by vtable instead of by name: the two of them (`0x46DA60` and `0x46DA10`, the
  classes `FUN_0041C5B0` and `FUN_0041C680` build) both keep the position at `+0x24/+0x28/+0x2C`,
  the angle at `+0x3C` and the length at `+0x40`, and each one's own draw (`FUN_0041D7C0` and the
  one at `0x41EA40`) reads the position and the angle at draw time as it writes them into the two
  animation objects the laser embeds, so moving them moves the beam. The velocity is read from each
  class (`+0x30/34/38` for the one that doesn't turn, whose length stops at `+0x434`; `+0x430/434/438`
  with the turn rate at `+0x440` and the length stopping at `+0x444` for the one that does). As in
  th11, a beam's length is not read at draw time (the update folds it into a sprite's pixel size
  once a frame), so a growing beam still extends in whole frames while its position and angle are
  smooth.

  The menus' text is the same problem the later games have, in an older shape: the text object is
  `[0x4776E0]`, its two record tables start at `+0x76C` (0x68 bytes a record, up to 0x100 of them)
  and `+0x6F6C`, and the counts of the ones the frame draws are at `+0x896C/+0x8970`, which the
  update clears once a frame; those two are kept where the game's own render runs and put back
  before every extra presentation, so an extra frame draws the same text the game's own frame did.
  th10 also has a **frame rate module of its own** (`\src\game\fps.cpp` is in the executable) whose
  task sits on the render chain, so an extra presentation would count three times the frames the
  game actually ran and, past 65, make the game resynchronise its clock. What that module keeps is
  saved where the game's own present runs and put back around every extra presentation (with its
  window's start pushed into the future so the extra frame doesn't settle the window itself), and
  its resynchronisation test is patched out the way thprac's th10 module does it (`EB 50` at
  `0x413516`), which is what keeps the game's own rate display honest once the rate is raised. It is
  also the first of the family to run part of the game on a **second thread** (the enemy thread
  `FUN_0041FAC0` starts with `__beginthreadex`), which is why what is projected is read and written
  at the one point in a frame where the game's own update walk has just run - the same point the
  game's own render reads those containers from.

  Confirmed in-game: the extras land evenly at 180 presents per second (`average: 3.000 presents per
  frame, 180.6 presentations per second`), with `1028 in play, 1028 projected` for the bullets in
  one run and `505 in play, 505 projected` in a later one, `1 body, 8 option and 42 shot sprite(s)
  projected` for the player, and `144 in play, 144 projected` for the lasers in a laser-heavy
  attack; nothing was left without a movement record and no projected position was written back
  during the render, which is what says the positions being moved are the ones the renderer reads.
  A replay recorded with the patch plays back on the unpatched game without desyncing, which is the
  outside check that the logic and the random numbers are untouched.

- **Touhou 11 (Subterranean Animism) gets frame interpolation too** (`26ac150`, `7d08bd2`, `7695746`, `af30cfb`)

  Subterranean Animism is the oldest of this batch (2008, a generation before Undefined Fantastic
  Object), so its offsets are its own: its frame code has the same **three render blocks and three
  present sites** (renders at `0x446193`/`0x446571`/`0x446730`, presents at
  `0x446382`/`0x4465D6`/`0x4467F9`), and one of the render blocks is inside the window update
  `CWindowManager__UpdateFast` (`0x446420`) itself, which is why the menus and the title screen get
  the extra frames too. Those six are what the extra presentations take over; the device is at
  `[0x4C3288]`, the render context is the object the block puts in `EDI` (`0x4C3280`), the sprite
  queue's owner is the one it puts in `ESI` (`[0x4C3268]`), `0x44FCE0` empties the queue,
  `0x44FD10` draws it and `0x456E10` renders the objects.

  Each render block presents from a small frame-end helper (`FUN_004461F0`/`FUN_004465D0`/
  `FUN_00446790`), all of them a 5 byte load of the device followed by 16 bytes of arguments, so
  all three are patched and the code after them runs with the present result. The two the block
  calls with their object in a register (`0x42A300` wants `EDI`, `0x44FD10` wants `ESI`) go through
  the same kind of `__declspec(naked)` trampoline th13 needed, with the caller's register put back
  before returning. th11 inlines the sprite queue's reset into the frame function, so the extra
  presentation calls `0x44FCE0`, which is the same thing.

  An extra presentation also has to put the picture's own state back: the viewport, the view and
  projection transforms (the game's frame builds them from its 2D context in `FUN_0042A810`, and a
  tool drawing its overlay replaces them with its own), the 2D context the frame rendered with
  (`[0x4C37CC]`, which the sprite queue's clipping reads) and the two values `FUN_0042A810` leaves
  in the sprite manager at `+0xB0`/`+0xB4` that place the queue on screen. All of them are taken
  where the game's own render runs and written back before every extra presentation (the game's own
  `FUN_0042A810` is not called again - it draws the queue once on its way in).

  The bullets are the manager `[0x4A8D68]` and its own six column lists (heads at `manager+0x14`,
  next at `bullet+0x4B8`): position at `+0x43C/+0x440/+0x444`, velocity at `+0x448/+0x44C/+0x450`,
  state at `+0x4B2` (1 is a full step, 2 and 3 are half of one) and the global slowdown at
  `[0x4A7948]`. The update divisor `[0x4C3466]` is read in seven places across the executable and
  written nowhere, and is 0, so there is exactly one update per rendered frame. The player is
  `[0x4A8EB4]`, its own position at `+0x87C/+0x880/+0x884` (its draw callback `FUN_00431BC0` copies
  that into the body animation object's drawn position every frame). Its options are 8 slots of
  `0xE4` bytes at `+0x7570` (position at `+0x5C/+0x60` as ints in 1/128 pixels, two animation
  objects at `+0xB0`/`+0xB4`) and its shots are 256 slots of `0x6C` bytes at `+0x96C` (position at
  `+0x14/+0x18/+0x1C`, objects at `+0x4C`/`+0x50`). Those two have their positions written into the
  objects' drawn position at `+0x3E8/+0x3EC` by the update, so it is the objects that are moved for
  them; the player's own position is moved directly. The objects are looked up by handle again
  every frame (by walking the sprite manager's two lists at `[0x4C3268]+0x7B562C`/`+0x7B5634`),
  which is what keeps the crash th12 had - a cached pointer to a freed sprite - from happening.

  The straight lasers are the manager `[0x4A8E94]` (list at `manager+0x18`, next at `laser+0x08`,
  state at `+0x0C`). th11's RTTI carries only `LaserLineInf` (vtable `0x494A64`) and
  `LaserInfiniteInf` (`0x494ABC`) - there is no curve or beam laser class in this game - and both
  keep the position at `+0x3C/+0x40/+0x44`, the angle at `+0x54` and the length at `+0x58`, all of
  which their draw (at vtable `+0x0C`) reads as it writes them into the two animation objects the
  laser embeds, so moving them moves the beam. The velocity is read from each class (Line:
  `+0x48/4C/50`, and it does not turn; Infinite: `+0x44C/450/454` with the turn rate at `+0x45C`),
  the length grows by `+0x60` up to `+0x450` (Line) or `+0x460` (Infinite). One known difference:
  a laser's length is not read at draw time in this game (the update folds it into a sprite's pixel
  size once per frame), so a beam that is growing still extends in whole frames even though its
  position and angle are smooth.

  The menus' text list - the problem described under the Undefined Fantastic Object entry below -
  is handled here from the start: the text object is `[0x4A8D58]`, its records are `0x130` bytes
  each (th11 is a generation older, so they are smaller than the later games' `0x138`) at `+0x87C`
  up to `0x140` of them, and the count of the ones the frame draws is at `+0x1847C`; the render
  hook keeps it and every extra presentation puts it back before it draws.

  Confirmed in-game: the extras land evenly at 180 presents per second (`average: 3.000 presents
  per frame, 180.6 presentations per second`); `549 in play, 549 projected` for the bullets, `1
  body, 8 option and 57 shot sprite(s) projected` for the player, and `256 in play, 256 projected`
  for the lasers in a laser-heavy spell card (the manager's own limit of `0x100`), with the picture
  and the pause and save screens normal.

- **Touhou 12 (Undefined Fantastic Object) gets frame interpolation too** (`c4b7811`, `fe9146f`, `fd8eddc`)

  Undefined Fantastic Object is the last of the games the requirements name (the earlier ones already
  had reverse engineering or notes to work from, and the later three were worked backwards from
  Double Dealing Character and Legacy of Lunatic Kingdom to here). Its frame function has the same **three render blocks
  and three present sites** (renders at `0x45017B`/`0x450529`/`0x4506C8`, presents at
  `0x450355`/`0x450586`/`0x450789`), and those six are what the extra presentations take over.
  `0x45A380` empties the sprite queue and `0x45A3C0` draws it, and both want their object in `ESI`,
  so they are called through the same kind of `__declspec(naked)` trampoline th13 needed - putting
  the object where the routine wants it and putting the caller's register back before returning.

  The bullets are the manager `[0x4B43C8]` and its own list of the ones in play: position at
  `+0x4BC`, velocity at `+0x4C8`, moved exactly the way the game's own update moves them (the global
  slowdown and the half step a starting bullet gets included). The player is `[0x4B4514]`, its own
  position at `+0x97C`. The player, its options and its shots are moved smoothly by the input and by
  each other with **no velocity to read**, so they are *measured* the way th13, th14 and th15 do it:
  what each moved by since the frame before is taken at the point the game presents its own frame,
  written in for the fraction of a frame an extra presentation sits at, and put back right after the
  render. The straight lasers are the manager `[0x4B44F4]`, position at `+0x50`, and read their
  velocity from their own class (see the item below).

  Confirmed in-game: the extras land evenly at 180 presents per second, no sprite is left without a
  movement record, and the bullets, the player, its options and shots and the straight lasers all
  move with the extra frames; the pause and the save-replay screens are as they should be. The
  enemies are not interpolated (their animation scripts place them and there is nothing to read),
  and the curve lasers are left alone.

### Changed

- **The four games' straight lasers read their velocity instead of measuring what they moved**
  (`7fdd8fd`)

  All four measured the lasers: the position each was at the frame before was kept **per object
  address**, and an extra presentation wrote in the difference times the fraction of a frame. The
  problem is not precision but **object recycling** - lasers are short lived, the game frees and
  allocates them constantly, and a fresh laser that lands in a dead one's slot gets "old position
  minus new position" as if it were a velocity, which draws that laser offset. A screen with a few
  hundred of them, which these games reach, runs into it almost every time. Reading the velocity the
  laser carries cannot do that (a warning line, whose velocity really is zero, is left where it is),
  and it also gets a laser's first frame right, where a measurement has nothing to compare against.

  Two things fell out of reading the classes: in th13, th14 and th15 a line laser's angle is fixed
  (their own updates never write it - only the infinite class turns), and their beam class's update
  function (`0x437FF0`, `0x443C50`, `0x44B7D0`) is a stub that just returns 0, so it never moves and
  has no layout of its own. The per class layouts of all four games are in the notes under
  `_analysis`, each read out of that game's own class update rather than copied.

### Fixed

- **The menus' text is no longer drawn twice by the extra presentations** (`d387341`)

  A menu, the pause screen and the save/load replay screens do not draw their text a line at a time:
  the game's own printf-for-text call **appends a record** to a list inside its text object (0x138
  bytes each, up to 0x140 of them) and **the drawing then walks the records the frame's count of
  them covers**. That count **is started over once per frame**, and that happens in the **update
  walk** - the walk an extra presentation does not run, because it is the game's logic. So an extra
  frame drew the records the game's own frame had left behind a second time, appended its own behind
  them, and drew the lot; and the **second** extra presentation of a frame draws into the same back
  buffer the game's own frame was presented from, so both copies land on one picture.

  The fields and the look of each game (all read out of that game):

  | Game | Text object | Records | **The frame's record count** | Look |
  |---|---|---|---|---|
  | th12 | `[0x4B43B8]` | `+0x97C` | `+0x18F7C` | the layout does not move between the two, so the copies sit **exactly on top of each other** and only make the text look a little denser |
  | th13 | `[0x4C2160]` | `+0xB5C` | `+0x1915C` | two copies of the line, **offset more and more along it** |
  | th14 | `[0x4DB520]` | `+0xBAC` | `+0x191AC` | as th13 |
  | th15 | `[0x4E9A58]` | `+0xC20` | `+0x19220` | as th12 |

  > **A note for anyone chasing this**: the symptom **cannot be seen at 120Hz** (one extra
  > presentation per frame is one step of this, which is too small), it shows from 180Hz on; and
  > comparing the game's own frame with the *first* extra alone shows almost nothing and looks fine.
  > Capturing **every presentation of a frame** is what shows it.

  The fix is the same discipline the bullets and lasers have always used - save, draw, put back: the
  count is saved at the start of the game's own run of the render walk (th13 also saves the twelve
  fields of its text object that its own walk was measured to change: the pointer the next record is
  taken from at `+0x040`, the position the text was left off at `+0x050/+0x054`, eight floats at
  `+0x3CC` and the count) and put back before every extra presentation. Each extra then appends its
  own records from the start of the list and draws exactly what the game's own frame drew, and the
  state it leaves behind is what a single run of the game's walk would have left, so the game's next
  frame starts the list over where it always did.

  What settled it was **writing the back buffer out as a BMP right before every present** (through
  `GetBackBuffer` and `GetRenderTargetData`, every presentation of a frame, not one of them) and
  comparing them: the game's own frame against its second extra differed in 2911 pixels with a mean
  channel delta of **109** (the doubled line, visible to the eye), while against the first extra it
  differed in 2262 pixels with a mean of **13.75**, which looks right. With this in, all four games
  come out **pixel for pixel identical**, and the screens' text was confirmed in-game to be back to
  normal.

## 2026-10-10

### Added

- **Touhou 13 (Ten Desires) gets frame interpolation too** (`43f9c66`, `94c39f1`, `740dd7d`)

  Ten Desires is the next one the requirements point at, and it runs on the same engine as th14, so
  this is the same mechanism with th13's own addresses - every one of them read out of the game
  itself. The frame function has the same **three render blocks and three present sites** (renders
  at `0x45CFFF`/`0x45D452`/`0x45D637`, presents at `0x45D207`/`0x45D4B6`/`0x45D709`), and those six
  are what the extra presentations take over. Two things are th13's own: two of the helpers the
  render block calls take their object in a register (`0x43B080` wants it in EDI, `0x4679A0` in
  ESI), which a function pointer cannot do, so naked trampolines put the object there and then
  call; and the middle present site zeroes esi among the arguments it pushes, so that site is
  hooked through a trampoline that does the same.

  The bullets are the bullet manager `[0x4C2174]` (built at a stage start, loading `bullet.anm`)
  and its own list of the ones in play (head at `+0x84`, node at bullet `+0x10`), with the position
  at `+0xB70`, the velocity at `+0xB7C` and the state at `+0xBBE`, moved exactly the way the game's
  own update `0x40DB10` moves them (including the half step a starting bullet gets and the global
  slowdown `[0x4C0A28]`). The player is `[0x4C22C4]`: its own position at `+0x5B8`, its options as
  8 slots of `0xE4` bytes at `+0xA2B8` (live dword at `+0x00`, sprite ids at `+0xB0/+0xB4`) and its
  shots as 256 slots of `0x9C` bytes at `+0x6A0` (live countdown at `+0x70`, sprite id at `+0x14`,
  position at `+0x2C`). The straight lasers are the manager `[0x4C22A0]` (head at `+0x18`,
  `obj+0x08` for the next one), and a vtable whitelist keeps only `LaserLineInf 0x4AB0CC`,
  `LaserInfiniteInf 0x4AB19C` and `LaserBeamInf 0x4AB204`; the curve `LaserCurveInf 0x4AB134` is
  left out. A line laser's own draw builds its picture out of the position at `+0x50`, the angle at
  `+0x68` and the length at `+0x6C`. The player, its options and its shots have no readable
  velocity and a laser turns and grows as it goes, so as in th14 and th15 these are measured: every
  game frame the fields are compared with the ones of the frame before, and the extra presentations
  move them by that difference times the fraction of a frame they stand for, putting them back
  right after the render.

  What took the work this time is which field has to move, because the game finishes its drawing at
  render time: `FUN_0046A700` bakes an animation object into the sprite manager's vertex queue as
  it draws (`0x467960` empties the queue, `0x4679A0` draws it in one call), so what matters is the
  field the bake reads. The bullets are copied from their position into their own animation objects
  by the draw pass (moving the position is what works); the options and the shots have their
  position written into the sprite's `+0x574` by the update, and the render only bakes the sprite
  (moving `+0x574` is what works); the player is the other way around - its draw callback
  `0x443D90` copies `player+0x5B8` into the body animation object's drawn position `+0x588` at
  render time, so the position itself is what moves it.

  Two leads the handover notes had were wrong, both of them things that were guessed from the shape
  of the code instead of read out of it, and both are corrected here: the "array of 0x44 byte shot
  objects" is the player's **bomb** (`BombReimuInf`, vtable `[0x4AA5BC]`, 8 objects of `0xD0`
  bytes that `0x40B4B0` builds) and has nothing to do with the player's shots, and the option slots
  start at `+0xA2B8`, not at `+0xA380` (that is `+0xC8` into each of them). The diagnostics now
  count the player's body, option and shot sprites apart and report the lasers, so a stage can be
  told apart from a menu at a glance.

  Confirmed in-game: the extra frames land evenly at 180.6 presentations per second on the 180hz
  display, no sprite is left without a movement record, and the player, its options, its shots and
  the straight lasers all follow the extra frames. The enemies are deliberately not interpolated
  (their animation scripts place them and there is no velocity to read) and the curve lasers are
  out of scope. Stage 1 of th13 is done with this.

## 2026-10-09

### Added

- **Touhou 14 (Double Dealing Character) gets frame interpolation too** (`8efd72f`)

  With th15 done, the other game the requirements name as worth starting from is th14, and the two
  run on the same engine - so this is the same feature with th14's own addresses, every one of
  them read out of the game itself (the disassembly and the decompiler) rather than by matching
  sprites or `.anm` names. The requirement is explicit about that: what has to be taken is the
  game's own containers, because projecting everything that looks like a bullet both stutters when
  a screen is full of items and gets the wrong objects.

  The frame function has the same shape th15's has - three render blocks and three presents, one
  for the game scene and one per menu scene (renders at 0x46A45D / 0x46A86D / 0x46AA0C, presents at
  0x46A67A / 0x46A8D6 / 0x46AAEC) - and those six call sites are what the extra presentations take
  over. The bullets are walked through the manager that loads `bullet.anm` when a stage starts
  ([0x4DB530]) and through that manager's own in-play list (head +0x80, node at bullet+0x10), with
  the position at +0xBC0, the velocity at +0xBCC, the state at +0xC0E and the hold flag at +0x20,
  moved exactly the way the game's own update (0x416700) moves them, half step and slowdown factor
  included. The player is at [0x4DB67C] (position +0x5E0), with its 8 options at +0xD6EC (0xE4
  bytes each) and its 256 shots at +0x6C8 (0xD0 bytes each) - none of which has a velocity to read,
  so they are measured frame by frame, the same way th15's are. The straight lasers are the
  manager at [0x4DB664] (list head +0x18, links at +0x08), reached through a vtable white list:
  LaserLineInf (0x4BE434), LaserInfiniteInf (0x4BE3CC) and LaserBeamInf (0x4BE364) are handled and
  LaserCurveInf (0x4BE2FC) is left alone, with the position at +0x54, the angle at +0x6C and the
  length at +0x70.

  Confirmed in-game: every bullet in play is counted as projected, the player with its options and
  its shots moves with them and keeps doing so after a stage is restarted, and the frames land
  evenly on the display (about 3 presentations per game frame, ~179 per second, on the 180hz
  display it was tested on). **The enemies
  are not done** - the requirements list them, but neither game has a velocity to move an enemy
  body by, its animation script decides where it is drawn, and guessing is the one thing the
  requirements rule out - and the curve lasers stay out of scope. The ini and both READMEs were
  changed to say the same.

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

### Fixed

- **Restarting a stage (ESC+R) could leave the player, its options and its shots unprojected**
  (`8efd72f`)

  A measured sprite keeps a record of where it was in the frame before, in a table keyed by the
  sprite's address, and those records were only ever added: nothing tells a sprite that is gone
  from a pool slot that is still up, so the shots of a stage fill the table up, and every sprite
  after them that isn't in it already stops being moved. The player itself is the first thing
  recorded in a frame, so a fresh stage always finds its own record - but restarting a stage
  builds the player at a new address, which needs a new record, finds none, and its projection
  quietly stops. That it only happened sometimes is the rest of the story: malloc puts the new
  player where the old one was often enough for the old record to be found and everything to work.
  The options and the shots go the same way as soon as their sprites move to addresses that aren't
  in the table.

  The records are recycled now: an empty slot is used first, and when there is none the record
  that was measured longest ago is taken, which has nothing to compare against anyway; a record
  that is in use by the frame being measured is never taken, so nothing that is on screen loses
  its place. The lasers get the same treatment, since they are handed out and freed one at a time
  and drift in the same way. th15's implementation had the same bug and gets the same fix. The
  diagnostics gained a `player: N sprite(s) this frame had no movement record` line, which stays 0.

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

- **The interpolation's rate readout moved next to the render time line in the bottom left
  corner** (`8efd72f`)

  It sat at the right edge of the play field, which was only picked to keep it clear of thprac's
  own frame rate and slowdown readout in the bottom right corner of the window. The patch already
  draws a line of its own in the bottom left corner - how long the render took - and both of them
  are its own readouts, so having them side by side is what makes them useful together. The
  overlay can be asked to follow that line now (`overlay_set_present_rate_follow_text`), and both
  games ask for it. Confirmed in-game in both.

- **The reference config that ships has the interpolation and the spin wait on by default**
  (`cb68840`)

  The ini that ships is the one most people end up running, so its values are the defaults in
  practice, and the two that matter most for what this fork is for were still at the values from
  before the interpolation existed: `Interpolation` was `0` (off, so the motion smoothing had to be
  found and turned on by hand) and `Sleep` was `1` (the vpatch-style wait, which costs less CPU but
  is slightly less precise than the spin wait). Both are what the games are actually played with
  now - `Interpolation = -1`, which picks the largest multiple of the game's frame rate the display
  can show, and `Sleep = 0`, the spin wait - and the built-in defaults are moved to match, so the
  "默认 / Default" line the reference config documents keeps being the value the patch uses when
  the file is missing. Setting `Interpolation` back to `0` turns the interpolation off.

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
