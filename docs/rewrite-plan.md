# UQM Rewrite Plan â `rewrite/core`

> **Superseded in part (2026-07-26).** Its game-side recommendation — port
> `uqm/ships/` and `uqm/comm/` verbatim, rewrite only the engine — is
> **withdrawn** by decision D9. See `game-rewrite-plan.md`.
>
> Kept because a reversed decision is more useful with its original argument
> attached, and because its *factual* findings are well cited and still hold:
> the `TFB_Random` unsigned-wrap bug, decode-affects-collision via
> `ModifySilhouette`, `cyborg.c` being the shared reference frame, Super Melee
> having 25 selectable ships, `libs/decomp` not being dead, and the ~62k-line
> deletion inventory. The engine-side plan (content pipeline, deletions) is
> unaffected.
>
> Its M1 (frame-exact CRC oracle) is void — see D10.

## Goal and constraints

Ship a single-threaded, SDL3, WASM-first UQM whose Super Melee is frame-exact against the current build. "Green-field" applies to the **engine** (`libs/`, 108k lines), not to the **game** (`uqm/`, ~129k lines). The 55k lines of content-as-code â 28 ship state machines, 27 race dialogue files, 30 world generators â are the game; they have no spec other than themselves, and rewriting them is unbounded downside for zero goal-relevant benefit.

Measured basis for all sizing (unchanged from the brief):

| tree | lines (.c+.h) |
|---|---|
| `libs/` | 108k |
| `uqm/` top level | 48k |
| `uqm/comm/` | 28k |
| `uqm/planets/` (incl. `generate/` 5.9k) | 21k |
| `uqm/ships/` | 15k |
| `uqm/supermelee/` (incl. netplay) | 12k |
| **total** | **237k** |

### Correction to the premise: `rewrite/core` forked away from the un-threading work

`rewrite/core` and `modernize/wasm-direct` are both `414f074ce`; merge-base with `modernize/wasm` is `a8e45101a`, i.e. **before** the 47-commit sweep. Counting blocking calls (`TaskSwitch|SleepThread|SleepThreadUntil|HibernateThread` across `sc2/src/uqm` + `sc2/src/libs`, excluding netplay and `libs/threads`): **`rewrite/core` 112, `modernize/wasm` 36**. `sc2/src/libs/coroutine.h` and `tools/unthread-burndown.sh` exist only on `modernize/wasm`.

I ran the merge in a throwaway worktree. **Exactly one file conflicts: `.gitignore`** (a 6-line hunk). `CMakeLists.txt` and `sc2/src/uqm/hyper.c` auto-merge. The staging report claimed "3 conflicting files"; its critique claimed 1; the critique is right and I verified it. The merge is an afternoon, and skipping it means redoing ~76 blocking-site conversions and re-finding three documented flattening bug classes. **This is step zero.**

Note the fork was deliberate, not accidental (`git reflog show modernize/wasm-direct` shows a hard `reset` to `a8e45101a` followed by cherry-picks of the vendoring commits). Confirm intent with whoever did it before merging â but merge.

---

## What we keep, port, and delete

### Keep verbatim â touching these is how the project fails

- `libs/math/` (368). `TFB_Random` at `random.c:62-70` is **not** ParkâMiller: `seed = A*(seed%Q) - R*(seed/Q); if (seed > M) return (seed -= M);` on `DWORD`=`uint32` (`compiler.h:33`). The unsigned wrap makes it produce `M+t+2` where textbook produces `M+t`. Diverges from correct Lehmer at ~draw 14 from seed 12345; cycle 802,451, not 2Â³Â¹â2. Copy it, bug and all, guarded by a 100-value golden vector asserted at startup.
- The integer trig: `sinetab[]` (`trans.c:23`), `SINE`/`COSINE` (`units.h:208-212`), `ARCTAN`, `square_root`. Dump `sinetab` to integers and commit them as a fixture â entry 8 is `0.773010*16384 = 12664.996`, inside float epsilon of an off-by-one.
- `intersec.c` + `TFB_DrawCanvas_Intersect` (`canvas.c:1996-2065`). Collision is a per-pixel read of the decoded surface that branches on `Amask` vs colorkey â I read it. **Image loading is gameplay code**, not renderer code. Commit `aa5de1681` ("sdl3: stop SDL3 clobbering the colorkey sentinel") already fired once in this class and was caught only by a cosmetic symptom.
- `SIZE` is `sint16` (`compiler.h:32,37`). Every file-backed struct and every velocity/position computation depends on the narrowing. Fixed-width typedefs with `static_assert`, no "cleanup to `int`".

### Port (transliterate CâC++, never rewrite)

`uqm/ships/` (15k), `uqm/comm/` (28k), `uqm/planets/generate/` (5.9k), and the sim core (`process.c`, `collide.c`, `velocity.c`, `gravity.c`, `init.c`, `ship.c`, `tactrans.c`, `cyborg.c`, `intel.c`, ~11k). Compile as C++ for RAII at the seams; change no evaluation order, no integer widths, no element ordering.

**`cyborg.c` (1,339 lines) is phase 0 of the ship port, not phase 27.** All 27 bespoke `intelligence_func`s delegate into `ship_intelligence`, it walks `disp_q` head-first and resolves target ties by first-encountered, and it calls `DrawablesIntersect` for lookahead (`cyborg.c:259`) â so the AI's decisions depend on sprite decode. It cannot follow the ships.

`sis_ship.c` (1,002) + `lastbat.c` (926) + `probe.c` (118) are **not** melee ships â they sit past `LAST_MELEE_ID` (`races.h:103-107`) and Super Melee has 25 selectable ships, not 28 (`meleeship.h:11-42`). That is 2,046 lines with no melee oracle. Budget them separately and honestly as hand-verified.

### Delete (measured, no behavioural exposure)

| target | lines | note |
|---|---|---|
| `libs/uio/` | 14,594 | â ~1k flat archive reader; only 51 `uio_*` symbols used externally |
| `libs/mikmod/` | 18,228 | â ~700-line ProTracker replayer; all 63 `.mod` are 4-channel `M.K.` |
| `libs/network/` + `supermelee/netplay/` | 9,717 | **keep `crc.c`+`checksum.c` (603) â that is the oracle** |
| `libs/graphics/sdl/` CPU scalers | 4,875 | `SDL_SetRenderLogicalPresentation` already does it (`sdl2_pure.c:193`) |
| `libs/video/` (DUCK FMV) | 2,365 | no `.duk` or `.aif` ships; base cutscenes are `.ani`+`.png` |
| `libs/threads/` + `libs/task/` | 2,183 | after the flip |
| `libs/cdp/` | 1,964 | provably dead â absent from `libs/CMakeLists.txt` |
| `libs/graphics/sdl/rotozoom.*` | 1,178 | one caller (`intro.c:625`); ~60 lines replaces it |
| `libs/sound/decoders/aiffaud+dukaud` | 1,268 | no content |
| `libs/decomp/` + `load_legacy.c` | 1,910 | **but see below** |
| `libs/callback/` + `libs/heap/` | 819 | one live user, `sis.c:1612`; `uqm/flash.c` (1,028) must become a per-frame update |
| `libs/graphics/dcqueue.c` | 928 | exists only to cross a thread boundary |
| `libs/list/` + `libs/md5/` | 784 | zero includers |
| `libs/sound/openal/` + `nosound/` | 988 | one mixer |
| **total** | **â62k** | |

Correction to the content report: `libs/decomp` is **not** dead. `load_legacy.c` is in the build (`uqm/CMakeLists.txt:41`) and `load.c:622,627` call `LoadLegacyGame` for pre-0.7 saves. Deleting it drops legacy save import â a policy decision, not dead-weight removal. We drop it; say so out loud.

### What we are deliberately NOT rewriting

1. **All 28 ship files, `cyborg.c`, and the sim core.** Evidence, re-read: Orz saves/overwrites/restores the ship's `ShipFacing`, `cur_status_flags`, `thrust_increment`, `max_thrust` around `inertial_thrust` (`orz.c:694-717`) and packs `turn_wait` as `MAKE_BYTE(delay,facing)`; Chmmr writes the *enemy's* velocity and then rewrites the enemy `STARSHIP`'s speed flags (`chmmr.c:389-409`) and deletes its own `preprocess_func` from the vtable (`chmmr.c:773`); Mmrnmhrm swaps its entire `CHARACTERISTIC_STUFF`; Druuge strips the enemy's `FIRES_*` flags around the shared AI call. Any schema expressive enough is C with extra steps. Extract only the 70-line `RACE_DESC` literal + constant block (~2,700 lines) as data.
2. **`uqm/comm/` stays as code.** I side against the gameplay report's DSL/IR recommendation. Reasons: its 16.1%-outside-the-vocabulary figure has no checked-in derivation and is the sole basis for the 9â11-week estimate; `LOCDATA`'s three per-race encounter function pointers (`globdata.h:97-102`) are unaccounted for in the IR; the Melnorme numeral grammar it budgets for hand-porting is already a `static const` table plus a shared engine interpreter (`commglue.c:123-200`); `PHRASE_ENABLED`/`DISABLE_PHRASE` scribble `'\0'` into loaded resource strings with conversation-scoped lifetime, which a "persistent flag" model changes; `shared_phrase_buf` is one global 2KB buffer aliased into the response list. The *stated motive* for the DSL â the fragile Duff's-device resume points â is solved for free by C++20 coroutines. And the corpus has no oracle: 23k lines that `docs/unthread.md` says have mostly never executed. Port it.
3. **The `.ani`/`.fon`/`.ct`/`.rmp` grammars.** The shipped content *is* the spec; the parser is the only written form of it. We move parsing to build time (below) but the semantics are copied, not redesigned.
4. **The element queue's insertion order.** Replacing the intrusive list with a vector is a separate, CRC-gated step *after* the transliteration is green, because order changes AI target selection.
5. **Internal resolution.** `ScreenWidth` is a runtime global (`units.h:31`) feeding `LOG_SPACE_WIDTH`, and `DISPLAY_ALIGN_X` is applied to raw `TFB_Random()` at `ship.c:477`. 320Ã240 is a gameplay constant. Output scaling is letterbox on the GPU only.

---

## Engine architecture

**Loop.** `SDL_AppIterate` per display frame: `Clock::beginFrame()` (one frozen `GetTimeCounter()` sample) â `Input::poll()` â `Audio::pump()` â `SceneStack::runFrame()` â `Screen::present()`. Delete `StartThread(Starcon2Main)` (`uqm.c:460`), `DoInput`, `RunScreenTask`, and the 250ms `HibernateThread` stall at `uqm.c:494`.

**Pacing â the load-bearing detail.** `battle.c:340-342` is `SleepThreadUntil(NextTime + BATTLE_FRAME_RATE/(speed+1)); NextTime = GetTimeCounter();`. Because the sleep wakes accurately this is effectively a fixed-deadline accumulator at 24 Hz. `BATTLE_FRAME_RATE = ONE_SECOND/24 = 35` **ticks**, and a tick is 1/840 s â the deadline is 41.67 ms, not 35 ms. On a 60 Hz rAF loop a naive `if (now >= next + period)` transcription lands on the third 16.7 ms tick = 50 ms = 20 Hz: a 17% global slowdown, browser-only, invisible natively because **there is no vsync anywhere in the tree**. Use `dueAt += period` with bounded catch-up (`while (now >= dueAt && steps < 4)`), clamp `dueAt = now` past ~4 periods. Set an explicit desktop throttle so the catch-up path is exercised at ~60 Hz natively, and add a CI check measuring achieved battle Hz.

**Input cadence â a real design bug in the obvious design.** `UpdateInputState` (`gameinp.c:246-249`) overwrites `PulsedInputState` wholesale each call. Sampling `_check_for_pulse` at display rate while a 15 Hz menu scene consumes it discards three of four edges â dropped keypresses, precisely the feel regression we are trying to avoid. Either derive Pulsed inside the gated scene step (preserving today's 1:1), or latch edges into a sticky accumulator the scene drains. Port `_check_for_pulse` (`gameinp.c:117-141`) and `_check_gestalt` (`gameinp.c:149-220`) verbatim; do not change their cadence.

**Scenes.** `StateDriver_Push/Pop/RunFrame` (`gameinp.c:393/410/423`) is already the target shape. `Scene { onEnter, frame(ctx) -> {Continue|Pop|Push|Replace}, onChildDone, onAbort }`. Push must be a tail action: today a nested screen is a recursive `DoInput` and the parent resumes after it; the code after the call moves into `onChildDone`. Three cross-cutting requirements the naive design misses:
- `PauseGame`, `SleepGame`, `ConfirmExit` are modal blocking loops living **inside** `UpdateInputState` (`gameinp.c:236-243`), reachable from every screen. They become engine-owned scenes the input layer pushes; every scene beneath must be pausable.
- `ScreenTransition` (`gfx_common.c:157-195`) is a blocking multi-frame loop called from **inside** `DoBattle` (`battle.c:324`). The battle scene is not a straight transcription of `DoBattle`.
- One `Rate` per scene is not enough. Comm runs ambient at `ONE_SECOND/40`, oscilloscope at `/32`, talk-anim at `/60`, plus subtitles off audio position. A scene needs a small set of independent gates.

**Rates do not divide evenly and that is part of the spec.** `ONE_SECOND/22` = 38, `/100` = 8, `/32` = 26 â the truncations are behaviour. Do not re-base the clock.

**Coroutines, not macros.** For the ~40 inline blocking sequences use C++20 `Task`/`co_await`. A coroutine frame is a compiler-generated heap allocation, not a stack switch â no Asyncify, no SharedArrayBuffer. This is measured-closed territory: `measure/asyncify` records whole-program Asyncify at 99.3% instrumentation / +35% compressed, and JSPI structurally cannot suspend a stack rooted below emscripten's JS main-loop frames. There is no retrofit. Keep `libs/coroutine.h` (recovered in the merge) as the C fallback notation.

**Renderer: keep the software blitter, present through `SDL_Renderer`.** 320Ã240 = 76,800 px/frame, ~4.6 Mpx/s worst case â free. `SDL_GPU` has no readback path for collisions or `LoadDisplayPixmap` (`loaddisp.c:26`) and nothing to accelerate. Target: blitter 2,854 â ~1,200 lines (5 primitives Ã 3 blend modes Ã {indexed, RGB}), plus the text path and the `WANT_ALPHA` target path that `primitives.c:153-290` special-cases â the "3 blend modes" framing understates the matrix (`gfxlib.h:295-321` documents REPLACE-with-alpha behaving as ALPHA on RGB targets except for Text).

**Demoted risks.** Palette mutation is *not* global-per-frame: fades never touch a colormap (`cmap.c:340-360` is a pure function of wall clock composited as a solid quad), and all 12 `XFormColorMap` call sites are under `uqm/comm/`, driven from `commanim.c`. Nested zips are not exercised: I checked all 584 `.ani` â **zero** start with `PK\x03\x04`, and `.fon` are directories. Keep the byte-span mount API, but justify it with "fetch a pack over HTTP into memory" (D6), not with a code path this content never takes.

**Audio.** One `SDL_AudioStream`, push model already implemented at `audiodrv_sdl.c:133-142`. 8 voices. Playback position becomes `submitted - SDL_GetAudioStreamQueued()` â exact, replacing the wall-clock estimate in `stream.c:670`. The sharp edge is comm: `FastForward_Page`/`FastReverse_Smooth` must reseek the decoder *and* flush the device queue within one frame, coupling three clocks a thread currently decouples.

---

## Content pipeline

**Write the Python packer first**, before the C++ VFS. It forces a reference reader for every format in the cheapest place to find byte-level surprises. Two of those surprises are already confirmed, and both would have shipped bugs:

1. **`.ct` has two shapes.** I parsed all 75 files: **79 entries are genuine PLUT runs** (`u8 start, u8 end, (end-start+1)Ã768 RGB`) and **123 are not**. `sc2/content/base/planets/banded.ct` is 3 entries of 386 bytes each, payload starting `80 ff â¦` â that is 128,255 as an *elevation* min/max, consumed by pointer arithmetic at `plangen.c:158-159` (`d = xlat_tab[d] - cbase[0]; ctab = (cbase+2) + d*3;`), never by `SetColorMap`. Expanding these as PLUT runs would read 98 KB out of a 386-byte entry and corrupt every planet surface. Discriminate on `entryLen == 2+(end-start+1)*768` vs `2+(end-start+1)*3`, and **assert** every entry matches one.
2. **Content is 100% CRLF.** I counted newlines across `.ani`/`.txt`: 25,491 CRLF, **zero** bare LF. Files open `"rb"` (`getstr.c:246`), so CRs reach the string table on every platform. Four sites depend on it, including `trackplayer.c:331,352` where `strcspn(text,"\r\n")` is what splits a phrase into subtitle pages. Do not "simplify" the line splitter.

**Layer precedence needs explicit priorities, not insertion order.** Content mounts `TOP` and `/packages/*.uqm` mount `BELOW` it (`options.c:335-337,358`) â "later wins" inverts base-vs-package. `uio_MOUNT_ABOVE` inserts immediately above the named handle (`mounttree.c:247-257`), so among several ABOVE mounts the **first** ends up highest. Model `TOP/BOTTOM/ABOVE-X/BELOW-X` as an integer priority assigned at mount time. Separately, `process_resource_desc` (`resinit.c:124-136`) does remove-then-re-add: **the `.rmp` index is a second overlay layer independent of the VFS**, ordered by `loadIndices` call order. Model it as such.

**Repack, measured:** 9,600 PNGs = 9.05 MB on disk; decoded raw = ~10 MB, deflate-9 â ~2.13 MB; only **82 unique palettes** exist. Grouping into ~96 load-unit blocks costs nothing (2.108 MB, marginally *better* than one stream), so lazy/streamed blocks are viable. Projected pack â 4.6 MB vs 13.73 MB today. Bake `.ani` descriptors, `.ct` runs (both kinds), `.xlt` as explicit `{int16 level[3]; uint8 xlat[256]}` (killing the raw-struct-cast landmine `plandata.h:200` warns about), string tables with name-hash indices, and font metrics as explicit numbers (`disp = extent+1` and the height-dependent `tune_amount` 0/â1/â2/â3, `gfxload.c:121-151`).

**Golden round-trip test must compare the colormap *slot*, not just pixels.** `tfb_prim.c:152-156` replaces a cel's embedded palette with `TFB_GetColorMap(colormap_index)` whenever `colormap_index != -1` â 6,443 of 6,984 cel lines. A test comparing pixels + embedded palette passes green while the slot binding is wrong.

**Also:** `res_GetResource` has exactly 6 callers, all immediately `res_DetachResource` â nothing is cached, every `LoadGraphic` re-decodes. Do not port 150 lines of refcount bookkeeping that buys nothing; preload archive blocks instead. Split the `res_Get/Put String|Integer|Boolean|Color` settings store out of the resource index into a plain `Config`.

---

## Fidelity harness

**This is the first code written, on the merged tree, before any engine work.** Without it "behaviourally identical" is unfalsifiable.

**The existing oracle is weaker than three of the five reports claim, and I verified it.** `crc_processELEMENT` (`checksum.c:107-131`) hashes nine `ELEMENT` fields. `crc_processSTATE` (`checksum.c:102-104`) hashes **only `val->location`**, not `image.frame` â so facing is invisible. Grepping the file for `STARSHIP|energy_counter|weapon_counter|special_counter|status_flags|hTarget|playerNr` returns **zero hits**. `crc_processSTAMP`/`crc_processINTERSECT_CONTROL` are `#if 0`. It is a netplay desync *detector* â it only has to catch divergence eventually, because ship-state divergence eventually moves an element. As a cross-implementation oracle it localises the bug several frames late, and a one-tick change confined to a `STARSHIP` counter may not trip it at all.

Fix before capturing any traces:
1. Move `crc.c`+`checksum.c` (603) to `uqm/statecrc.c`, out from under `#ifdef NETPLAY` and the `getNumNetConnections() > 0` gate at `battle.c:268`. Delete `crc_processBytes` (`crc.c:82-87`, never increments `buf`; no callers).
2. Add `crc_processSTARSHIP` over `race_q[0..1]`: `ShipFacing`, `cur_status_flags`, `old_status_flags`, `weapon_counter`, `special_counter`, `energy_counter`, `ship_input_state`, `crew_level`, `ship_info.energy_level`. Add `next.image.frame`, `playerNr`, `hTarget` (as pool index), and the element pool index `(h - pq_tab)/object_size` as a delimiter so field misalignment cannot alias.
3. Defer the function-pointerâid registry: ~158 distinct `*_func`s, all `static` in per-ship TUs. The STARSHIP counters catch most "wrong branch, hasn't moved yet" cases at a fraction of the cost.

**Hooks.** Record at `battle.c:176` â `PlayerInput[cur]->handlers->frameInput(...)`, the vtable at `battlecontrols.c:33-52` â **not** `gameinp.c:509`, which is the human-only path. Replay is a fourth `BattleInputHandlers`. Tape format must include ship selection (`selectShip` is multi-round) and the `battleEndReady` flag. Seed: melee seeds at `melee.c:1316`, and `battle.c:404`'s reseed explicitly *skips* SUPER_MELEE â the "battle seed at `battle.c:384`" in one report does not exist.

**Pin these in the scenario header, all verified as sim-affecting:** `optMeleeScale` (â `opt_max_zoom_out` â `PreProcessQueue` scroll â `PostProcessQueue` writes `next.location` at `process.c:966`, which is inside the CRC), `ScreenWidth`/`ScreenHeight` (â `LOG_SPACE_WIDTH` â every spawn position), seed, fleet, planet type.

**Corpus.** 25Ã25 melee pairs Ã 4 seeds cyborg-vs-cyborg â 2,500 scenarios, no recorded input needed â but three corrections to the "it's free" claim, all verified:
- `nth_frame` is written **only** at `encount.c:807,829`, gated on `CYBORG_ENABLED` in the full game. Super Melee never sets it, so there is no fast-forward from melee; the harness must wire it.
- `intel.c:46-48` reads the live keyboard even in cyborg mode (`InputState |= CurrentInputToBattleInput(...) & BATTLE_ESCAPE`). Pin `CurrentInputState` to zero; do not merely refrain from typing.
- `MeleeGameOver` (`pickmele.c:649-685`) spins 4 real seconds per battle and `selectShipComputer` (`pickmele.c:277-288`) 0.5 s per pick. Short-circuit both, or the corpus takes hours.
- Add a max-frame cap and a distinct "stalemate/timeout" verdict.

**Do not give the AI its own `RandomContext`** (the `// TODO` at `battlecontrols.h:63`). It shares the global stream today; splitting it is a gameplay change and invalidates every trace. Decided, closed.

**`DrawablesIntersect` returns 0 when `!ContextActive()`** (`intersec.c:245`) â but `ContextActive()` is just `_GraphicsStatusFlags & CONTEXT_ACTIVE`, set by `SetContext`, which `RedrawQueue` does every frame. The failure mode is *stubbing the graphics library*, not running windowless. Run with `SDL_HINT_VIDEO_DRIVER=dummy` and a real graphics stack, and assert a known scenario produces a nonzero collision count.

**Second oracle: `dumpUniverse`** (`uqmdebug.c:759-800`) over all 502 systems pins `planets/` + `generate/`. `SysGenRNG` is a *different* generator (`random2.c`) from `TFB_Random`; the universe dump compares output, not seeds, so it holds â but state that the two oracles have disjoint blast radii. Freeze the RNG contract at `gendefault.c:136-149` (analysis â snapshotâBIO â life â snapshotâMINERAL â minerals â reuseâENERGY) and **keep the double `GenerateDefault_generatePlanets` at Maidens** (`genvux.c:71-78`) until the dump proves it inert.

**Third: `SaveGame()` byte-diff** (`save.c:733-800`) â no wall-clock data, so replay-then-save-then-`cmp` is a whole-adventure check. But it is **blind to RNG stream position** (the seed is never saved), so pair it with a seed digest.

**CI guard:** grep for `GetTimeCounter|SDL_GetTicks|time(NULL)|clock()` in `ships/`, `process.c`, `collide.c`, `cyborg.c`, `tactrans.c`, `init.c`, `weapon.c`, `gravity.c`, `velocity.c`. It is currently **zero hits** â the sim has no wall-clock dependence at all. That invariant is the reason any of this works; make it a build failure to break it.

---

## Milestones

**M0 â Merge (1 day).** `git merge modernize/wasm` into `rewrite/core`; resolve `.gitignore`. Recovers 47 commits, `libs/coroutine.h`, `tests/coroutine_test.c`, `tools/unthread-burndown.sh`. Also land `measure/asyncify`'s doc updates â `docs/unthread.md Â§9.5` still says the Asyncify decision is "not yet done" when `c99d25780` took it and rejected the scoped bridge. **Exit: same playable build, burn-down 36, wasm target works.**

**M1 â Oracle (2 weeks).** Extended `statecrc`, replay vtable, harness CLI, corpus generator, universe dump, `SaveGame` diff, CI on Linux + Windows. Do **not** promise the wasm gate here: pixel-exact collision needs the sprites decoded, so a wasm headless run needs the full content mounted â that lands at M4. **Exit: ~2,500 golden CRC streams reproduced bit-for-bit; a deliberate one-tick change to `WEAPON_WAIT` *and* to a `STARSHIP` counter both fail.**

**M2 â Burn-down to zero and the flip (6â10 weeks).** Finish `lander.c`, `pl_stuff.c`, the ~26 `GenerateFunctions` implementations, `battle.c`, `menu.c`, `getchar.c`, plus the graphics/sound leaves. Then delete `Starcon2Main`, `DoInput`, `libs/threads`. **This milestone is the project goal; everything after is optional.** Serialize by subsystem, one owner, one PR â flattening rewrites control flow in place and a conversion in file A can change input-edge timing in file B. Be honest: M1 observes maybe 6â8 of the remaining sites; the rest are menus and screens gated by playthrough plus the burn-down counter, not by the oracle.

**M3 â WASM without headers (2 weeks).** Drop `-pthread`, drop COOP/COEP from `tools/wasm-dist.sh`. Plain static hosting.

**M4 â Content pipeline (4 weeks).** Python packer â `.uqmpak`; `libs/uio` 14,594 â ~1k; drop `--preload-file`/MEMFS's 10,554 nodes. Add the wasm replay gate under node here. Add a synthetic `shadow-content` addon fixture â no shipped addon exercises the ABOVE path.

**M5 â Engine collapse (6 weeks).** DCQ â immediate; scalers, rotozoom, mikmodâProTracker, audio push mixer, input on `SDL_Gamepad`. Record the 15-opcode draw-command stream (`drawcmd.h:26-43`) before the change and diff it. Render all 63 modules through old mikmod first and spectrally diff the replacement; keep a trimmed mikmod (`load_mod`+`mplayer`+`virtch`, ~5.3k) in the budget if the diff is bad.

**M6 â Game port (12â16 weeks, parallel with M4/M5).** `cyborg.c` first, then ships easyâhard (supox, syreen, spathi, arilou, human â urquan, chenjesu, shofixti, mmrnmhrm, androsyn â pkunk, melnorme, chmmr â orz), each gated on a green CRC trace. Then `generate/` hybrid (table the declarative half, port the procedural half) gated on the universe dump. Then comm, gated on a reinstated `UQM_DEBUG_COMM` forced-conversation walk plus `tools/comm-switch-scan.py`.

**M7 â Deletion sweep.** netplay, cdp, video, decomp/legacy-save, OpenAL. ~62k lines total across M4âM7.

---

## Risks and open questions

**What could sink this, ranked.**

1. **Melee divergence found late.** Mitigated only by M1, and only if the CRC is extended first. If M1 slips or the extended oracle cannot reproduce its own traces on the unmodified tree, stop â that is the falsification signal, and it is cheap to reach.
2. **Image decoder changes gameplay.** Collision reads decoded pixels; `ship.c:475-482` and `ModifySilhouette` (`weapon.c:263-285`, reached from `vux.c:195`) are *rejection loops* against those masks, so a one-pixel decode difference changes the `TFB_Random` draw count and desynchronises everything downstream. `ModifySilhouette` means even the HUD icon art is sim-critical. Add a startup CRC over every decoded frame's hotspot/width/height/alpha mask as a separate, earlier-failing test.
3. **Comm coverage.** 28k lines that `docs/unthread.md` says mostly "sits behind story state no save has", with no oracle. This is the largest untested surface in the tree and the harness does not touch it. Reinstate `UQM_DEBUG_COMM` (reverted at `dbd811362`) as permanent test infrastructure before any comm work.
4. **Comm animations consume the global RNG on a wall-clock schedule** (`commanim.c:398-400` stepping on elapsed ticks, drawing `TFB_Random()` at `:50,59,67,91`). That is the same stream that drives encounter composition, so RNG position after a conversation is a function of real elapsed time â which breaks adventure replay and silently poisons the `SaveGame` oracle. Driving comm animation off a tick counter is a decision to take **now**, and it is a deliberate behaviour change we should record.
5. **Unsequenced evaluation order.** `misc.c:175-184` hoists two `TFB_Random()` calls into temporaries with a comment saying argument order varies per system. No same-expression double-draws remain today. C++ leaves function arguments unsequenced; any refactor can reintroduce one. This is likelier than the `long`-width hazard in `SINE` (which I could not make overflow â max 1.07e9, inside int32).
6. **Browser pacing.** See Engine architecture. Native testing cannot reproduce it.
7. **Scope creep into `ships/`/`comm/`.** C++ makes rewriting them feel like progress. Hard boundary: C++ only in `engine/`; game code is transliterated.

**Open, and needing a decision before the milestone that depends on it.**

- **Is "identical" frame-exact or player-indistinguishable?** I have assumed frame-exact for battle and generation â it is the only version with an oracle. Rule on it before M1 defines its pass criterion.
- **Saves.** Do existing `.sav` files have to load? We assume no (drops `libs/decomp` + `load_legacy.c`, 1,910 lines) but the `GameState` bit layout stays regardless because comm's entire memory is those 1,238 bits.
- **3DO addons.** 133 MB, and the 3dovoice pack **replaces six races' dialogue text outright** (arilou, melnorme, mycon, starbase, syreen, utwig point at `addons/3dovoice/<race>/<race>.txt`). So "is voice in scope" is not a clean cut â the phrase tables themselves change. Decide before M4.
- **Full-game battle seed** comes from `GetTimeCounter()` and is never saved, so the same encounter fights differently twice. Preserved behaviour, or derive from saved state? Genuine design question.
- **The double `generatePlanets` at Maidens** (`genvux.c:71-78`). One universe-dump run answers it. Do it before anyone is tempted to tidy it.
- **`libs/graphics/widgets.c`** (1,163) is the setup menu â game UI living in `libs/`, whose option set (CPU scalers, gamma â already a no-op at `sdl2_common.c:107`) is exactly what we are deleting. Decide at M2, not M5; it is how you change settings during bring-up.
- **`res_DetachResource`'s documented contract** ("a NEW copy will be loaded next time") must be preserved or every call site audited, if we add a real cache.