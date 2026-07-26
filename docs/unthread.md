# Un-threading UQM — the map

Goal: one OS thread, DOS-style flat game loop — the shape the original DOS
Star Control II actually had (threading arrived with the 3DO-lineage code
UQM derives from). This enables a WASM build with **no pthreads, no
SharedArrayBuffer, no Asyncify, no special headers — a plain main loop**,
and simpler debugging everywhere. Upstream's own TODOs (`uqm.c:370`,
`uqm.c:434`) already declare the threads a wart to be removed.

Revision note (2026-07-25): an earlier version of this map used a
stackful-coroutine bridge (`THREADLIB_COOP`). Dropped by owner decision —
direct flattening is preferred, because every conversion can land under the
still-threaded build (see §3), making the bridge redundant. Coroutines
remain only as the §9 fallback.

---

## 1. Inventory (verified @ `d6583f225`)

**Thread spawn sites — exactly three:**
| Site | Thread |
|---|---|
| `src/uqm.c:445` `StartThread(Starcon2Main…)` | game logic — the target |
| `src/libs/sound/stream.c:792` | audio stream decoder |
| `src/libs/sound/mixer/nosound/audiodrv_nosound.c:149` | `--sound=none` pump |

Plus the SDL/OpenAL audio-device callback thread (driver-internal).

**Blocking surface:** ~120 `SleepThread`/`SleepThreadUntil`/`TaskSwitch`/
`HibernateThread` sites across 48 files; `WaitCondVar` only in
`dcqueue.c:58`; semaphores only in dcqueue/tfb_draw "sendsignal" + threadlib
internals; `WaitThread` only in the reap path.

**Shape of the game code — the decisive facts:**
- **~60–70% is already per-frame callback code.** Interactive screens are
  `InputFunc` state functions invoked once per frame by the blocking
  trampoline `DoInput` (`src/uqm/gameinp.c:360` — `do { Async_process();
  TaskSwitch(); UpdateInputState(); …InputFunc… } while`). Measured: 48
  `DoInput` call sites / 31 files; 106 `InputFunc` references / 39 files.
  `Battle()`'s loop body is likewise already one frame.
- **The rest is inline blocking sequences** — intro/credits/FMV, comm talk
  animations, lander launch/descent, explosion/fade loops:
  `for(…){draw; SleepThreadUntil(…);}` mid-function with live locals.
  Estimate 40–60 such sequences.

## 2. Target architecture

```
SDL_AppInit:     current init path
SDL_AppEvent:    input/event handling
SDL_AppIterate:  1. Async_process(); UpdateInputState()
                 2. game_state_stack top → update(frame)   // one frame
                 3. PumpStreamDecoder(); audio mix+push (§5)
                 4. TFB_FlushGraphics()                    // drain DCQ, present
SDL_AppQuit:     uninit path
```

- **Explicit game-state stack** replaces the recursive `DoInput` nesting:
  entering a screen pushes a state frame (its `InputFunc` + context struct);
  returning pops. The main loop calls only the top of stack, once per frame.
- Pacing: states that ran slower than the display rate (battle at 24 Hz via
  `BATTLE_FRAME_RATE`) keep a `next_due` time and no-op on early frames;
  bounded catch-up for stalled tabs.
- The draw-command queue can stay initially (it's harmless and keeps diffs
  small); once single-threaded it degenerates to a same-thread command list
  and can later be simplified to direct drawing. The `TFB_WaitForSpace`
  condvar and "sendsignal" semaphore waits disappear with the thread.

## 3. Strategy: direct flattening under the threaded scaffold

The enabling insight: **a per-frame state machine is callable from
anywhere** — including from the current threaded `Starcon2Main`. So each
conversion (one screen, one cutscene) lands as an ordinary PR on a build
that still runs threaded, fully playable, A/B-testable against unconverted
behavior. The thread is not removed first; it is removed **last**, when the
burn-down (§7) reaches zero. There is no big-bang and no bridge layer.

Consequences:
- No coroutine library, no Asyncify/fiber dependence, no yield-semantics
  subtleties, no coroutine stack sizing. WASM gets a plain main loop.
- The conversion work is pattern-heavy and mechanical — suited to
  AI-assisted sweeps (this was the miscalibration in the earlier estimate:
  2–4 months of human grind is days-to-weeks of assisted conversion). The
  binding constraint is **behavioral QA**, addressed by landing
  incrementally on a playable build.

## 3a. Notation: stackless coroutines (`libs/coroutine.h`)

Owner decision (2026-07-25): the §4 transformation is written with a small
Duff's-device macro layer rather than hand-rolled phase switches. `UQM_BEGIN`
/ `UQM_YIELD` / `UQM_SLEEP[_UNTIL]` / `UQM_WAIT_UNTIL` / `UQM_CALL` /
`UQM_RETURN` / `UQM_END`. Locals still get hoisted into a context struct —
that part is unavoidable — but the original control flow survives, so a
conversion diff reads as "the same function, with its locals moved".

This is **not** the §9 fallback. Nothing switches stacks; a task compiles to
a plain C function that returns to its caller at each yield. WASM still gets
a plain main loop.

Each converted function `Foo` becomes `FooCtx` + `Foo_Init` + `Foo_Frame`,
plus a **transitional blocking wrapper** keeping the original name and
signature (`Foo_Init`; `UQM_RUN_BLOCKING (Foo_Frame (&c))`). The wrapper is
why converting a function costs nothing at its call sites: a call site only
changes when its *own* function is converted, and then it becomes `UQM_CALL`.
When the last wrapper is gone, so is the game thread.

Rules (each fails loudly, not silently): live state goes in the context; at
most one yielding macro per source line (`case __LINE__` — duplicates are a
compile error); no yield inside a `switch` of your own. `tests/coroutine_test.c`
pins the resume semantics under CTest.

Screens need one extra piece, in `gameinp.c`: `GameFrame_Begin()` is the
per-frame preamble `DoInput`'s loop body used to run inline (async pump,
input state, menu sounds, input hook). A flattened screen no longer has a
"call me once per frame" entry point, so the preamble has to be driven
separately — `RunScreenTask()` does it for a converted screen with an
unconverted caller, and after the flip the main loop just calls it first
thing each iteration.

## 4. Conversion patterns

1. **`DoInput` screens → state frames.** Hoist each `InputFunc`'s
   surrounding locals into a context struct; `DoInput(x)` call sites become
   `PushGameState(inputFunc, ctx)`; the trampoline's per-iteration body
   (menu sounds, input update) moves into the shared driver. Nested screens
   (solarsys → starmap → menu → confirm) nest naturally on the stack.
2. **Inline blocking sequences → mini state machines.** Live locals into a
   struct; each `SleepThreadUntil` becomes a state/phase advance with a
   `next_due`. These are the 40–60 real rewrites (comm animations, FMV,
   lander sequences, credits, fades).
3. **Setup → loop → teardown functions:** teardown moves to state-exit
   hooks; `CHECK_ABORT` early-outs map to unwinding the state stack (pop
   until a handler claims it) — mirrors the existing semantics where
   aborts propagate up through nested `DoInput` returns.
4. **`Starcon2Main` outer state machine** (`starcon.c:154`) is already a
   dispatch loop over activities — it becomes the bottom-most state frame.

## 5. Audio (real code changes, independent PRs)

1. **MixSDL → push model:** replace the audio-callback pull with SDL3
   `SDL_AudioStream` push — each iterate, if queued < ~100 ms, run
   `mixer_MixChannels` on the main thread and `SDL_PutAudioStreamData`.
   After this, the device thread is SDL-internal and touches zero game
   state — the modern equivalent of a DOS ISR draining a ring buffer.
   Works under the threaded build too; land early.
2. **Stream decoder → pump:** `StreamDecoderTaskFunc`'s loop body
   (`stream.c:536-579`) is already one-shot per iteration — extract as
   `PumpStreamDecoder()` called from the main loop; delete the task.
3. **nosound pump:** same trivial treatment.
4. **OpenAL backend:** unnecessary for WASM once push-MixSDL works; keep on
   desktop only if free (revisits D5).

## 6. Sync primitives end state

After the flip, threadlib shrinks to nothing: mutexes/semaphores/condvars
become no-ops with debug assertions (single thread ⇒ no contention
possible), then call sites are deleted in cleanup sweeps. `SleepThreadUntil`
survives only inside the state-driver as "set `next_due`". Timer base
(`ONE_SECOND=840` over `SDL_GetTicks`) is unaffected.

## 7. Order of work (every step lands green and playable)

1. MixSDL push conversion (§5.1) + decoder pump (§5.2) — under threads.
2. State-stack driver + convert the `DoInput` trampoline and 2–3 pilot
   screens (e.g. restart menu, starbase) to validate the pattern.
3. Sweep the remaining `DoInput` screens (mechanical, batched PRs).
4. Sweep the inline sequences (comm, FMV, lander, credits, fades) —
   the QA-heavy part; one subsystem per PR with a playthrough check.
5. **Burn-down gate:** `tools/unthread-burndown.sh`, run by CI with a
   `--max` budget that only ever moves down. It counts raw blocking
   primitives *and* `UQM_RUN_BLOCKING` wrappers — both are places the call
   stack must survive a frame boundary, which is exactly what Emscripten
   cannot do. Exempt: `libs/threads/**`, `coroutine.h`, the driver's own
   `TaskSwitch` in `gameinp.c`, netplay (compiled out), `uqm.c`.
6. **The flip:** run the state driver from `SDL_AppIterate`; delete the
   `Starcon2Main` spawn, `ProcessThreadLifecycles`, and the threadlib
   backends; keep a tagged pre-flip threaded build for regression bisects.
7. Cleanup: no-op primitive deletion, DCQ simplification (optional).

## 7. Where this stands (2026-07-26)

**Burn-down 44**, from 106 at the start of the sweep. Build clean, `ctest`
green (`coroutine`, `response`), `tools/comm-switch-scan.py` clean.

Landed since the §7a snapshot below:

- **§7b.1 driver-side pacing.** Screens hand a deadline to the state driver
  instead of sleeping. The deadline lives in the driver's `StateFrame`, *not*
  in `INPUT_STATE_DESC` as originally prescribed — the derived structs disagree
  on their layout past `InputFunc`.
- **`UQM_DOINPUT`** (controls.h): a coroutine can drive an *unconverted* screen,
  so a caller stops blocking without its callees being converted first. This is
  what made the rest affordable.
- **The `gameopt.c` cluster** — `GameOptions`, `PickGame`, `SettingsMenu`.
- **§7b.2 in full.** `CommRunTask`/`DoCommTask` (an `InputFunc` hands the screen
  to a task and gets it back), and `RESPONSE_BEGIN`/`RESPONSE_SEGUE`/
  `RESPONSE_DELAY` (`uqm/commresponse.h`) — a resume point for `RESPONSE_FUNC`s,
  which are `void`-returning vtable entries and cannot become tasks. **All 45
  race-file `AlienTalkSegue` calls are gone.**

### What still has to happen

1. **The remaining 44 sites.** Largest clusters: `lander.c`, `pl_stuff.c`,
   `genpet.c` (the `GenerateFunctions` vtable — `generateOrbital`/`pickupEnergy`,
   the other half of §7b.2, ~26 implementations), plus `battle.c`, `intel.c`,
   `menu.c`, `getchar.c`, and the graphics/sound leaves (`dcqueue.c`,
   `vidplayer.c`, `sound.c`, `gfx_common.c`).
2. **`UpdateInputState` hoist** — unblocks `PauseGame`/`SleepGame`/`ConfirmExit`,
   which still run their own input loops inside the input preamble (§7a).
3. **`HibernateThread (ONE_SECOND/4)` at `uqm.c:494`** — a 250 ms browser
   main-thread stall on focus loss.
4. **Netplay** (`supermelee/netplay/`, 3 `DoInput` sites) — exempt from the
   gate, Phase 5.
5. **The flip (§7.6)**: delete `Starcon2Main`'s thread, `UQM_RUN_BLOCKING`,
   `RunScreenTask`, `DoInput`, and the `TaskSwitch` in the driver.

### What to be careful of

- **A resume point inside your own `switch` compiles fine and is wrong** — the
  label binds to that switch, so on resume nothing matches and the rest of the
  function is silently skipped. The compiler catches a *missing*
  `RESPONSE_BEGIN` (a `case` outside a switch) but not this. That is what
  `tools/comm-switch-scan.py` exists for; run it after any comm sweep.
- **One global resume point** means a function with one must not call another
  that yields. Safe today only because every such call passes a ref that cannot
  reach the callee's segue — see the rule in `commresponse.h`.
- **Most converted comm code has never been played.** It sits behind story state
  no save has. `UQM_DEBUG_COMM` (reverted from this branch, see the commit
  before this one — `git revert` it back to use) forces any conversation and was
  used to confirm the Zoq-Fot-Pik path end to end. Reinstate it before the next
  comm sweep; `melnorme` is the highest-value run still outstanding.

## 7a. Status (2026-07-25)

Burn-down: **47** (from 106 at the start of the sweep; `tools/unthread-burndown.sh`).

Done:
- §5.1/§5.2 audio push + decoder pump, §7.2 state-stack driver (earlier).
- `libs/coroutine.h` + `tests/coroutine_test.c` (§3a).
- `util.c`: `WaitForAnyButton[Until]`, `WaitForNoInput[Until]`, `PauseGame`,
  `SleepGame` are tasks; six blocking wrappers remain for their callers.
- `gameinp.c`: `GameFrame_Begin()` split out of `StateDriver_RunFrame`,
  `RunScreenTask()` added.
- `restart.c` **fully flattened** (9 sites → 1): `DoRestart` → `Restart_Frame`
  (the §7.2 pilot screen), `RestartMenu`, `TryStartGame`, `StartGame`. Only
  `StartGame`'s wrapper is left, for `Starcon2Main`'s `while (StartGame ())`.
  Verified on Windows: menu renders, 30 frames per ~897 ticks, i.e. the
  `ONE_SECOND/30` pacing the threaded build had.

Two things the pilot surfaced, both now written into §3a:
- A converted screen has no per-frame entry point, so the `DoInput` preamble
  had to become callable on its own (`GameFrame_Begin`).
- `DoRestart`'s `switch (pMS->CurState)` had a `SleepThreadUntil` in its
  `QUIT_GAME` arm. A yield cannot live inside the task's own switch, so the
  switch became an if/else chain. Expect this in other screens.

Cutscenes (§7.4), first pass:
- `fmv.c` fully flattened (7 → 2). `Introduction` and `Victory` kept no
  wrapper — restart.c was their only caller and is a task now. `SplashScreen`
  and `DoShipSpin` still have one, for starcon.c and shipyard/buildpick.
- `credits.c` fully flattened (2 → 0), wrapper and all: restart.c is again
  the only caller. `DoCreditsInput` became a task the same way `DoRestart`
  did; `on_input_frame` stays registered with `SetInputCallback` and keeps
  being driven from `GameFrame_Begin`.
- `intro.c` **fully flattened** (5 → 0), wrapper and all. `DoPresentation`
  became `Presentation_Frame`; `ShowSlidePresentation`, `FadeClearScreen`,
  `DoVideoInput`, `ShowLegacyVideo` and `ShowPresentation` are tasks around
  it. The context types moved to a new `intro.h` because fmv.c's three
  cutscene tasks now embed a `ShowPresCtx` and reach the presentation through
  `UQM_CALL` — so no cutscene blocks internally any more.

Three things the intro pass surfaced:
- `Present_UnbatchGraphics`'s `TaskSwitch` was a yield inside a helper, which
  a stackless task cannot have. It now returns "the caller should yield" and
  the eleven call sites do `if (Present_UnbatchGraphics (c)) UQM_YIELD (c);`.
- Resuming into the middle of the opcode loop means the loop's own locals are
  gone, so `Opcode` and `pStr` moved into the context; `MOVIE`'s `fps/from/to`
  are consumed before its yield instead.
- The `CALL` opcode makes a presentation *recursive*, and a context struct
  cannot contain itself. The nested frames live in a file-static stack
  (`MAX_PRES_DEPTH`), which is sound because presentations are modal. Shipped
  content nests two deep: `cutscene/ending/ending.txt`.

### The pacing-sleep input bug (found and fixed)

**The main menu had not accepted keyboard input since the pilot conversion.**
Two causes, both now fixed.

*Fixed:* `StartGame`'s wrapper used `UQM_RUN_BLOCKING`, which only calls
`TaskSwitch`, so nothing ran `GameFrame_Begin` and `UpdateInputState` never
ran at all. It and `DoShipSpin` now use `RunScreenTask`, which had been
written for exactly this in the pilot pass and never wired up. **A task
reached from an unconverted caller must be driven by `RunScreenTask`, not
`UQM_RUN_BLOCKING`**, unless it only polls through `AnyButtonPress` (which
calls `UpdateInputState` itself — why `SplashScreen` was unaffected).

*The deeper one:* `PulsedInputState` is **edge-triggered**.
`_check_for_pulse` (`gameinp.c:116`) reports a key only on the frame where
`cached && !old`; on every later frame it returns 0 until the repeat delay
elapses. `DoInput` called `GameFrame_Begin` exactly once and then immediately
called the `InputFunc`, so the screen saw every edge. A *flattened* screen
paces itself — `Restart_Frame` ends its loop with
`UQM_SLEEP_UNTIL (c, c->TimeIn + ONE_SECOND / 30)` (`restart.c:301`) — and
every yield inside that sleep is another `RunScreenTask` iteration, hence
another `UpdateInputState`. The edge is consumed by one of the pacing yields
and the screen resumes to find `PulsedInputState` back at 0.

Instrumented on Windows, holding Down for 400 ms across ~11 menu frames:

		RSTDBG t=227 imm=1 cur=1 pulse=0     <- and identically for t=228..237

`ImmediateInputState`/`CurrentInputState` carry the key the whole time;
`PulsedInputState` never fires. So the preamble *is* running now (the
`RunScreenTask` half worked) and the pulse is being eaten.

This was never specific to `restart.c` — it hits **any** converted screen
that sleeps for pacing, which is most of them, so it had to be settled before
the §7.3 sweep rather than after.

**The fix (owner decision, 2026-07-25): pacing yields are distinct from frame
yields.** `UqmStatus` gains `UQM_PACING`, which the `UQM_SLEEP*` family
returns and `UQM_CALL` propagates (via `_coro.sub`, so a sleep deep in a call
chain still reaches the driver). The driver runs `Async_process()` for a
pacing yield but only runs the full preamble — `UpdateInputState`, menu
sounds, input hook — for `UQM_PENDING`. That restores `DoInput`'s invariant
exactly: **one input sample per screen frame, taken immediately before the
screen reads it.** `tests/coroutine_test.c` case 8 pins the distinction and
the propagation.

The one thing to watch was `credits.c`: its scroll is driven by the input
hook, which now ticks once per credits frame instead of once per display
frame. It is fine — `processCreditsFrame` is self-rate-limiting through its
own `NextTime`, and `CreditsInput_Frame` already sleeps at exactly
`CREDITS_FRAME_RATE`.

The gate now counts `RunScreenTask` alongside `UQM_RUN_BLOCKING`, so trading
one bridge for the other cannot appear to be progress.

**A second `UQM_CALL` hazard, found the same way.** The `CALL` opcode held
its sub-context in a local (`SlidePresCtx *sub = &nestedPres[c->depth]`).
Resuming jumps to the case label *inside* `UQM_CALL`, past that initializer,
so every frame after the first ran on a garbage pointer — the whole ending
cutscene was a black screen. Arguments to `UQM_CALL` are re-evaluated on each
resume and must therefore be rebuilt from context state, not cached in a
local. Written up as a rule in `libs/coroutine.h`.

Verified on Windows, all by driving the real game:
- main menu accepts input (tapped past three times its inactivity timeout);
- New Game runs the intro presentation — `title.ani`, `TFI` fades, `ANI`,
  `DRAW`, `WAIT`/`DSYNC` pacing;
- `Escape` aborts a presentation and the game proceeds correctly into Sol,
  which also exercises `SlidePres_Frame`'s teardown;
- the `CALL` opcode: all three of `ending.txt`'s sub-presentations
  (`victory1` → `victory2` → `final`) enter and return through
  `nestedPres[]`, including a `MOVIE` opcode;
- the credits path (splash + scrolling credits via the menu timeout), which
  had never been exercised before.

Still unexercised: 3DO video (`ShowLegacyVideo`) — it needs the 3DO content,
which is not in the base pack.

Two notes for whoever tests this next, both of which cost real time here:
- The menu timeout is 20 s, not 120 s — `InactTimeOut` is `(hMusic ? 120 :
  20)` and the music addon is not installed. An idle menu drops into
  splash+credits fast enough to be mistaken for a screen you thought you
  selected. Never conclude "my keypress worked" from a screen change alone.
- Injected keys need real scan codes (SDL3 maps by scancode, so
  `keybd_event(vk, 0, …)` is silently dropped), a ~150 ms hold, and genuine
  foreground.

§7.3 started, one screen in:
- `setupmenu.c` (2 → 1). `DoSetupMenu` and `SetupMenu` merged into a single
  `SetupMenu_Frame` — the `initialized` flag a re-entered InputFunc needed
  becomes the entry chunk before the loop, and the setup/teardown that
  bracketed `DoInput` folds in around it. No wrapper: `restart.c` was the only
  caller, and its `SetupMenu ()` — which blocked *inside* an already-converted
  task, a bridge the gate never counted — is now a `UQM_CALL`. The one
  remaining site is `OnTextEntryFrame`, which belongs to `getchar.c`'s text
  entry screen and goes when that is converted.
  Verified by driving it: main menu → Setup opens, arrow keys move the
  highlight, Escape tears down and returns to the main menu.

§4.4 done: **`Starcon2Main` is a task.** It was already a dispatch loop over
activities, so the conversion is small — the body becomes
`Starcon2Main_Frame`, `while (StartGame ())` moves its test into the loop
body (a `UQM_CALL` cannot live in a loop condition), and the game thread
drives it through one `RunScreenTask` at the bottom of `starcon.c`.

This was sequenced ahead of the starbase cluster deliberately. The root being
unconverted was what forced *every* callee to carry a counted blocking
wrapper, so conversions were coming out roughly net-zero on the gate:
`starbase.c` alone would have removed 3 sites and added 2 wrappers. With the
root converted, callees drop their wrappers instead. It paid for itself
immediately — `restart.c` 1 → 0 and `fmv.c` 2 → 1, because `StartGame` and
`SplashScreen` are now reached with `UQM_CALL`.

`StartGame` exposes `StartGame_Init`/`_Frame`/`_Result` over a file-static
context rather than exporting five nested context types through `restart.h`;
one game is being started at a time, and the tree is far too big for the game
thread's 1024-word stack. `Starcon2Main`'s own context is file-static for the
same reason.

Verified by driving it: boot → splash → main menu → Enter starts a new game
→ the intro presentation runs → Escape aborts it and the game comes up in Sol.
The Setup branch of the menu was driven too (open, arrow-key navigation,
Escape back out), since it reaches `SetupMenu_Frame` through the new root.

Super Melee (§7.3), top-down:
- `Melee ()` is a task. Its screens are a **flat** `InputFunc` state machine —
  `DoMelee`, `DoEdit`, `DoConfirmSettings`, `DoConnectingDialog` and
  `loadmele.c`'s `DoLoadTeam` swap `pMS->InputFunc` between themselves and
  return TRUE, with one `DoInput` dispatching whichever is current — so the
  task **inlines that trampoline** instead of nesting `UQM_CALL`s. Expect the
  same shape in `starbase.c`, which swaps `InputFunc` the same way.
- Each of those InputFuncs ended by pacing itself with `SleepThreadUntil
  (TimeIn + ONE_SECOND / 30)`. Rather than converting five functions to
  tasks, the deadline moves to `pMS->NextFrameTime` and the dispatch loop does
  the waiting. Same deadline, same per-function rate, and none of them blocks
  any more: five sites for a handful of lines. Zero means "no pacing", which
  the netplay branch of `DoConfirmSettings` still needs.
  Measured on the melee screen afterwards: 0.03 s CPU over 6 s wall, so the
  loop is throttling rather than spinning.

Left in that cluster: `melee.c` 3 (`StartMelee`'s two fades, and
`DoConfirmSettings`'s mid-function netplay sleep, which is not in tail
position so it cannot move to the caller), `pickmele.c` 3, `buildpick.c` 1.

Not started: the ~30 remaining `DoInput` screens (§7.3), the rest of the
inline sequences (§7.4) — comm, lander, melee — and the flip. The starbase
cluster (`starbase.c` 3, `shipyard.c` 2, `outfit.c` 1, `pickship.c` 1) is
deferred pending a save game to reach it with; it is also entangled with
`comm.c`, since `VisitStarBase` is mostly `InitCommunication` calls.

**Open design question for the flip:** `PauseGame`/`SleepGame`/`ConfirmExit`
are invoked from inside `UpdateInputState()`, i.e. from the middle of the
per-frame preamble. Once the main loop drives the root task they cannot
block there. They are already tasks; the likely answer is for the main loop
to run them as a modal state that suspends the game task, rather than
calling them from the preamble. Decide before §7.6.

**Known gap in the gate (§7.5):** an unconverted `DoInput` screen adds nothing
to the count unless its `InputFunc` happens to sleep, because `DoInput`'s
frame boundary is the exempt `TaskSwitch` in `gameinp.c`. Zero is therefore
necessary but not sufficient — the ~48 `DoInput` call sites have to go too.
Folding them into the count would jump the number by roughly half again;
worth doing, but as its own change with its own budget bump.

## 7b. Two structural changes that would make the rest cheap

A per-blocker inventory of everything still outstanding lives in
`unthreading-blockers.md`. Read that first if you are picking up the work;
this section is the why, that file is the what.



Learned across the 106 -> 53 sweep, neither done yet. Recorded because both
change the *shape* of the remaining work rather than chipping at it.

### 7b.1 Put `NextFrameTime` in `INPUT_STATE_DESC` (done, with one deviation)

Nearly every site removed so far went by one pattern: an `InputFunc` ending
`SleepThreadUntil (X); return TRUE;`, where the sleep is pure pacing and lifts
into the caller's dispatch loop. It is cheap, creates no context struct,
hoists no locals, and dodges the locals-across-yield bug class entirely.

Its one limitation is the precondition: the lift needs the caller to *already
be a task with a dispatch loop*. That single constraint produced everything
else — the strict top-down ordering, the starbase cluster deferred twice,
`pickmele.c` pricing out at net-zero, and the `Melee`/`Battle`/
`ExploreSolarSys` root conversions existing mostly as enablers.

`controls.h` already documents that every screen state is "a struct derived
from `INPUT_STATE_DESC`", and the `BOOLEAN (*InputFunc)` prefix is shared.
Add a second common field and have `DoInput` and `StateDriver_RunFrame` wait
on it exactly as the converted loops do. Two loops, one field; purely
additive, since converted loops already behave this way.

Then every remaining tail-position pacing sleep is a one-line edit, in any
order, **with no caller conversion first** — a leaf under an unconverted
parent stops being stuck, and the ordering rule stops mattering for the
largest class of sites.

**It must land together with fixing the gate** (§7.5's known undercount):
`gameinp.c` is wholly exempt, so pacing moved into `DoInput` would vanish
from the count while `DoInput` still blocks. Count the ~48 `DoInput` call
sites in the same commit and accept the number jumping up.

**What actually landed, and why it differs.** The field is *not* in
`INPUT_STATE_DESC`. The derived structs do not agree on a layout past
`InputFunc`: `MENU_STATE` puts `SIZE Initialized` where a second common field
would have to sit, `TEXTENTRY_STATE` something else again. Reading a wake time
through an `INPUT_STATE_DESC*` cast would therefore read `Initialized` on
about twenty struct types — an unchecked layout contract whose failure mode is
a screen sleeping for hours, and one the compiler cannot police because the
casts are already there.

So the deadline lives in the driver's own `StateFrame`, and
`StateDriver_PaceUntil (pInputState, wakeTime)` records it only when
`pInputState` is the screen the driver is currently running. Everything §7b.1
promised still holds — the lift is one line, needs no context struct, and does
not require the caller to be converted first. The context check buys something
the field could not: a screen whose caller is a coroutine dispatch loop rather
than the driver gets an honest `FALSE` back, so `DoMenuChooser` can pace both
ways from one call site:

```c
if (!StateDriver_Pace (pMS, ONE_SECOND / 20))
        SleepThread (ONE_SECOND / 20);
```

Five sites lifted this way. Also landed alongside: `UQM_DOINPUT` (controls.h),
which lets a coroutine drive an *unconverted* screen — the screen keeps its
`InputFunc` untouched and the macro's yields become its frame boundaries, so a
caller can stop blocking without its callees being converted first. That is
what made the `gameopt.c` cluster affordable: `SettingsMenu` and `PickGame`
became tasks while `DoSettings` and `DoPickGame` stayed exactly as they were.
`GameOptions` itself could not use it — every branch of `DoGameOptions` enters
another screen, and an `InputFunc` has nowhere to yield to — so its trampoline
is inlined and the submenus are `UQM_CALL`s, the shape hyper.c already had.

The resume label had to move from `__LINE__` to `__COUNTER__` for any of this:
a macro expands on one line, and `UQM_DOINPUT` contains two yields.

### 7b.2 Generalise the deferred-modal trick, for the hard tail

What now blocks progress is **indirect dispatch, not depth**: comm's
`RESPONSE_FUNC`s and `GenerateFunctions`' `generateOrbital` / `pickupEnergy`
(~26 implementations, gating `pl_stuff.c`, `genpet.c` and `lander.c`'s
`KillLanderCrewSeq`). A leaf under a vtable cannot become a task without
changing the vtable signature and every implementation.

The hyperspace pass solved exactly this shape: `sis_hyper_postprocess` was
three plain C frames below the task, so it now raises a request and
`Battle_Frame`'s loop runs the menu once the stack is flat.

**The mechanism, generalised (landed).** `ENCOUNTER_STATE` grew a deferred-task
slot and comm.c grew two functions:

```c
CommRunTask (pES, task, ctx);   // hand the screen to a task, return TRUE
static BOOLEAN DoCommTask (ENCOUNTER_STATE *pES);  // pump it, hand it back
```

An `InputFunc` cannot yield — it is a plain per-frame call — so instead of
blocking it makes itself *not the current screen* for a while. `DoCommTask`
becomes the `InputFunc`, advances the task one frame per dispatch frame, and
restores the original `InputFunc` when the task returns `UQM_DONE`. This is a
generalisation of something comm.c had already grown by hand: the
`SelectConversationSummary` / `…Done` pair does exactly this handover for one
specific screen, with `SummaryReturnFunc` playing the part of
`TaskReturnFunc`.

Pacing needs no special handling. A task waiting out a delay keeps returning
`UQM_PACING` from inside its own `UQM_SLEEP`, and the dispatch loop yields once
per iteration anyway, so the wait costs one frame each and spins nothing.

The semantic subtlety the sweep has to respect is that **anything after the
blocking call has to move to where the `InputFunc` is re-entered.** That is
cheap for a call in tail position and not cheap otherwise — which is what makes
the inventory below matter more than the site count.

**The comm inventory, measured rather than estimated.** The earlier "~29 sites
across 15 race files" is one number over three quite different shapes. There
are 26 `AlienTalkSegue` calls in the race files, plus one in comm.c itself:

| | sites | what follows the segue | status |
|---|---|---|---|
| driver | 1 | the rest of `DoCommunication` | done — `CommRunTask` |
| tail | 9 | non-blocking state updates only (`AddEscortShips`, `SET_GAME_STATE`, `Response`, `PrepareShip`) | done — `RESPONSE_SEGUE` |
| paired | 10 | a visual cue between two segues — `XFormColorMap` in all five | done — `RESPONSE_SEGUE` |
| not a response func | 6 | two `init_encounter_func`s, two helpers | done — see below |
| messy | 1 | `melnorm.c` — `DrawCargoStrings`, two `SleepThread`s, a countdown loop | done — `RESPONSE_DELAY` |

**All of it is converted.** `AlienTalkSegue` has no callers left in the race
files; the blocking wrapper in comm.c survives only until the flip.

The one in comm.c is the one that matters at runtime: `DoCommunication` runs
`AlienTalkSegue (WAIT_TRACK_ALL)` on nearly every conversation frame, so it was
by far the most-executed blocking call in the game. It is now a task.

The other 26 run once each, on specific story beats. The shape that works for
them is not a continuation but a **resume point**, and it is the same one for
the tail and paired groups — `RESPONSE_BEGIN` / `RESPONSE_SEGUE` /
`RESPONSE_END` in `uqm/commresponse.h`. The `RESPONSE_FUNC` signature and its
registration are untouched; the function simply gets a `switch`-on-saved-pc, so
`AlienTalkSegue (n)` becomes a yield and comm.c re-enters it after the segue.
Duff's device jumps past everything before the label, which is precisely the
required semantics — the `NPCPhrase` calls and `PLAYER_SAID` tests must not run
twice. `tests/response_test.c` pins that down.

Two things bite, and both did:

- A **local live across the yield** is not preserved. `rebel.c`'s `Rebels`
  computes `NumVisits` from `EscortFeasibilityStudy` before the segue and hands
  it to `AddEscortShips` after; it is a file static now.
- **The `RESPONSE_REF` parameter counts as such a local.** `talkpet.c`'s
  `MindControl` reassigns its own `R` before the segue and uses it after — and
  the driver re-passes the ref it originally dispatched with, not the
  reassigned one. Silent wrong-branch bug if missed.

The paired group needed nothing the tail group did not, which is the point of
having built a resume point rather than a continuation: `RESPONSE_SEGUE (1)`,
the `XFormColorMap`, `RESPONSE_SEGUE ((COUNT)~0)`, in the original order. The
one new thing it exercises is comm.c's driver loop going round twice, where
`UQM_CALL`'s resume label is reused — a label is a source position, not a loop
iteration. `tests/response_test.c` drives the real macro shape for that case.

**Six of the 26 are not response functions at all.** Both counts in this table
before this point got that wrong, by grepping for `AlienTalkSegue` rather than
looking at what encloses it — first missing three, then missing `starbas.c`'s
three as well. The lesson is cheap and worth writing down: *the call site does
not tell you the category; the enclosing function does.*

- `rebel.c`'s and `orzc.c`'s `Intro` are `init_encounter_func`s, called from
  `HailAlien_Frame` before the dispatch loop starts. **Done**, and it cost
  nothing new: the vtable signature stays `void (*) (void)`, the two functions
  take an ordinary `RESPONSE_SEGUE`, and `HailAlien_Frame` drives them with the
  response driver via a thunk that drops the unused ref. Being a task already,
  it uses a plain `UQM_CALL` rather than a `CommRunTask` handover.
- `starbas.c`'s `DiscussDevices` and `zoqfotc.c`'s `ZFPTalkSegue` are plain
  helpers called *from* response functions. A resume point cannot span a
  function boundary — the `switch` is per-function.

**`ZFPTalkSegue` was much bigger than its one site suggests, and is done.** It
was one *definition* with **45 call sites across 7 functions**. The segue sat in
tail position inside it, so it became a macro: `RESPONSE_SEGUE` now expands
directly into each caller and all 45 call sites are textually unchanged.

What made it a sweep rather than a one-line change was that **13 of the 45 sat
inside a `switch (NumVisits++)`**, in `ZoqFotHome` and `Intro`. A resume label
inside your own switch binds to *that* switch, so `ResponseResumePoint`'s switch
would have had no matching case, fallen through to nothing, and silently skipped
the rest of the function. Those switches are if/else chains now, keeping the
post-increment exactly (`which = visits++`, and the last arm undoes it), and
three variables that were live across a yield — `ZoqFotHome`'s `NumVisits` and
`KnowMask`, `Intro`'s `NumVisits` — are file statics.

Two properties made this safe to do in bulk, and both are worth relying on
again:

- A `RESPONSE_SEGUE` with no enclosing `RESPONSE_BEGIN` **fails to compile**
  (a `case` outside a switch), so no caller can be forgotten.
- A `RESPONSE_SEGUE` inside the wrong switch **compiles fine**, so the compiler
  is no help there. That one needs a brace-depth scan over the file — grep for
  the call and check whether a `switch` is open at that point. Run it after the
  sweep, not before.

`ZoqFotInfo`'s `InfoLeft` looks live across a segue and is not — it is assigned
below the last one.

`starbas.c`'s `DiscussDevices` went the same way as `ZFPTalkSegue`: the Vux
Beast report was its tail, so it became a `DiscussDevicesReport()` macro
expanded in `NormalStarbase`, with `VuxBeastIndex` promoted to a file static so
it survives the return. Only the `DiscussDevices (TRUE)` call reaches the
report, and that caller discards the result, so nothing had to be carried back.

`melnorm.c`'s `DoSell` needed one new thing: **`RESPONSE_DELAY`**, the same
resume point waiting out a plain duration instead of a talk segue. That is what
a `SleepThread` sitting in the middle of a response function becomes, and the
driver picks between the two by whether a duration was set.

### The bug this sweep uncovered

Converting `zoqfotc.c` introduced, and the `starbas.c` work exposed, a fault in
the mechanism itself. `Intro` calls `AquaintZoqFot (0)` on the line after a
segue. On resume the global resume point still held *Intro's* label, so
`AquaintZoqFot`'s own `switch` matched nothing, fell through, and skipped its
entire body — registering no responses at all. A dead conversation on the first
Zoq-Fot-Pik meeting.

The rule as first written — "must not call another function that *yields*" —
had the wrong boundary. A callee that merely *has* a `RESPONSE_BEGIN` was
already enough to break. The fix is that `RESPONSE_BEGIN` now **consumes** the
resume point rather than reading it, so any nested call sees 0 and starts from
its own top. `tests/response_test.c` pins it, and reverting the consume fails
exactly the two nested-call checks.

Worth remembering as a class: a mechanism keyed on one global is fine until two
users of it are live at once, and "these functions call each other" is easy to
miss when the calls look like ordinary tail calls.

### Reaching these conversations at all

Most of the converted comm code sits behind story state no save file has, so it
had unit-test and compile coverage but nothing had ever *run* it. `UQM_DEBUG_COMM`
fixes that: set it to a race name and that conversation runs once, as soon as
you are in flight.

```
UQM_DEBUG_COMM=zoqfotpik ./uqm        # then load any save
```

`zoqfotpik` is the one that matters most — its `Intro` calls `AquaintZoqFot` on
the line after a talk segue, which is the nesting case that registered no
responses at all until `RESPONSE_BEGIN` was made to consume the resume point.
**If the response list appears, that path is genuinely exercised**; if the aliens
talk and the conversation closes by itself, the callee registered nothing and
the bug is back. Confirmed working. `melnorme` is the next most valuable — it is
the only site outside the unit test that exercises `RESPONSE_DELAY` and a paired
segue together.

Two things about the hook that cost time and are easy to repeat. It must not arm
from `Starcon2Main`, and it must not arm while the main menu is up: the main loop
pumps `debugHook` on its *first* iteration, which on a load is before the save is
in place, so the conversation runs against an SIS that does not exist and the
game dies on load. It arms from `UpdateInputState` and only once
`LOBYTE (GLOBAL (CurrentActivity))` is `IN_HYPERSPACE` or `IN_INTERPLANETARY`
with no `CHECK_LOAD` pending.

## 8. Risks

| Risk | Mitigation |
|---|---|
| Subtle timing regressions in converted animations/cutscenes | incremental landing on a playable build; A/B vs pre-conversion tag; per-subsystem playthrough checklist |
| Hidden blocking site missed before the flip | CI burn-down grep (§7.5) is the gate, not judgment |
| `CHECK_ABORT` unwind semantics diverge from nested-return semantics | pilot screens (§7.2) prove the pattern before the sweeps; abort paths get explicit tests |
| A subsystem resists flattening (deeply intertwined blocking) | scoped stackful bridge for that subsystem only (§9.4), gated on the Asyncify measurement (§9.5) |
| DCQ behavior differences once producer/consumer are same-thread | keep DCQ initially unchanged; simplify only after the flip is stable |

## 9. The coroutine-backend alternative

Recurring question, worth settling in writing: instead of converting the game
code, could the *threadlib* be reimplemented on coroutines, leaving the ~700
source files alone? (Note: the earlier revision of this file claimed the full
`THREADLIB_COOP` design was recoverable from git history. It is not — the
doc's first commit `25beeced9` already carries the revision note, so only the
decision survived, never the design.)

### 9.1 Stackless as a backend: structurally impossible

Not merely hard. A stackless coroutine can only suspend **in its own frame** —
it compiles to a function that returns to its caller. UQM calls
`SleepThread()` ten-odd frames deep (`Starcon2Main` → `StartGame` →
`DoRestart` → `ShowPresentation` → `DoPresentation` → `SleepThread`). For the
innermost to suspend, every frame between it and the scheduler has to be a
coroutine that propagates the suspension.

That propagation *is* the conversion; `UQM_CALL` is exactly it. So "swap
threadlib for stackless coroutines" is not a cheaper route to the same place,
it is a restatement of §4. There is no shortcut hiding here.

### 9.2 Stackful as a backend: possible, mechanical, and taxed on WASM

Entirely feasible. `threadlib.h` is a thin layer — `Thread`/`Mutex`/
`Semaphore`/`CondVar` are all `void *` behind ~15 entry points. A cooperative
scheduler over real stacks satisfies all of them: `SleepThread` yields until a
deadline, `TaskSwitch` yields, mutexes become no-ops (single thread),
`WaitCondVar` yields until signalled. Natively that is minicoro / `ucontext` /
Win32 fibers — days of work, **near-zero churn in the game code**, and all 86
burn-down sites plus all ~48 `DoInput` call sites stop mattering at once.

The bill comes due on the platform this port exists for. Switching stacks
needs an addressable call stack and wasm's is not:

| Mechanism | Status |
|---|---|
| Asyncify | Whole-program CPS transform; everything that can transitively reach a suspend point gets instrumented, which for UQM is most of the game. Costs size and speed. |
| `emscripten_fiber` | Built *on top of* Asyncify — same bill. |
| JSPI | Much cheaper, but Chromium-led; non-Chromium availability is the open question against the Phase 3.7 three-browser matrix. Verify current status before relying on it. |
| WasmFX stack switching | Still a proposal. |

That is precisely the tax §1 exists to avoid.

### 9.3 What the conversion has actually cost so far

The honest case *for* the backend, from the intro.c/restart.c sessions: three
bugs in two files, and all three were artifacts of flattening that a stackful
backend would not have had.

- The pacing/edge-trigger input bug (§7a). Under a stackful backend `DoInput`
  keeps its shape, so every screen still gets exactly one `UpdateInputState`
  per `InputFunc` call. That bug was a semantic divergence *created by*
  flattening.
- The `CALL` local-pointer bug, and the `Opcode`/`pStr` hoisting before it.
  Stackful preserves locals for free — the whole "live state goes in the
  context" rule evaporates.

So the cost of flattening is not typing, it is a class of subtle behavioural
bugs. Against that: each is a once-per-class discovery, now encoded as rules
in `libs/coroutine.h` and pinned by `tests/coroutine_test.c`; and the
remaining bulk (§7.3, ~34 screens) is the cheap kind, already per-frame.

### 9.4 The scoped bridge — the option actually worth holding open

Not either/or. Flatten the bulk, and host *only* the subsystems that resist in
fibers, using `ASYNCIFY_ONLY` to bound instrumentation to that call graph
rather than the whole binary. Realistic candidates are the inline-sequence
heavyweights: `comm.c`, `lander.c`, `melee.c` (six sites each). That keeps a
plain main loop for nearly everything and pays Asyncify only where it buys
something.

Rejected variant: threads on desktop, flattening only for wasm. Two divergent
control flows means every behavioural bug gets chased twice.

### 9.5 Decision, and the measurement that should drive it

Current decision: **stay with direct flattening** — the conceptual hard part
is done and the remaining majority is mechanical.

But §9.4 should be settled by a number, not a debate: **build one Asyncify
wasm and measure artifact size and frame time on the real content.** That
single measurement decides whether the bridge is a reasonable home for
comm/lander or a non-starter, and it is worth having *before* the §7.4 sweep
commits to flattening them by hand. Not yet done.
