# Un-threading WIP review

Date: 2026-07-25. Reviewed at `668f4a53f` (`modernize/wasm`), against the plan
in `docs/unthread.md`. Everything below was checked against the actual tree
and commit trail, not taken from the plan's own status section.

## Verdict

The approach is sound, and the execution is better than the plan promised.
Direct flattening under the still-threaded scaffold was the right call for
these constraints, and the trail of commits shows it working exactly as
designed: every conversion landed on a playable build, the two genuinely
subtle bugs the transformation can produce were caught by *playing the game*,
and both are now encoded as rules and pinned by tests. I would not change
course. The recommendations below are about sharpening the remaining half,
not redirecting it.

## What the trail verifies

- **The burn-down is real and honest.** `tools/unthread-burndown.sh` reports
  79 today; the doc claims 79 from 106; CI pins `--max 79` — the budget has
  been ratcheted to the exact current count at every step, so any regression
  fails the build. Counting the `UQM_RUN_BLOCKING`/`RunScreenTask` bridges
  alongside raw primitives closes the obvious way to game the metric, and the
  script documents its own known undercount instead of hiding it.
- **Semantic parity was checked where it matters.** `UQM_SLEEP_UNTIL` always
  yields at least one frame even when the deadline has passed — and that is
  exactly what upstream `SleepThreadUntil_SDL` does (it degrades to
  `TaskSwitch()` on a passed deadline, `sdlthreads.c:301`). The pacing-vs-frame
  yield distinction (`UQM_PACING`) reconstructs `DoInput`'s
  one-input-sample-per-screen-frame invariant precisely, and
  `tests/coroutine_test.c` pins it.
- **The sequencing is being steered by the metric, correctly.** Converting
  `Starcon2Main` before the starbase cluster — because an unconverted root
  made every callee conversion net-zero on the gate — is the kind of
  mid-course correction that shows the gate is actually driving decisions
  rather than decorating them.
- **Cheap patterns are being found instead of grinding.** The melee cluster's
  five `SleepThreadUntil (TimeIn + ONE_SECOND/30)` tails became one
  `NextFrameTime` field plus a wait in the dispatch loop — five sites for a
  handful of lines, no new tasks. `Battle_Frame` reuses the same trick.
  This matters for the ~30 remaining screens: the sweep cost estimate holds
  only if this kind of shortcut keeps being taken.
- **Verification discipline is unusually good** for a solo/AI-assisted effort:
  runtime checks drive the real game on Windows (menu input past the timeout,
  the intro presentation, `ending.txt`'s nested `CALL` presentations, the
  credits path), and the doc records what was *not* exercised (3DO video).

## Findings, in order of importance

### 1. The §9.5 Asyncify measurement is still not done — do it next

This is the highest-leverage open item, and it is cheap. `comm.c` (6 sites)
and `lander.c` (6) are the QA-heaviest flattenings left: talk animations and
lander sequences are exactly where a subtle timing divergence hurts most and
is hardest to A/B. One day spent building an Asyncify wasm and measuring
artifact size and frame time settles whether those two files should be
flattened by hand at all, or hosted in a scoped `ASYNCIFY_ONLY` bridge
(§9.4). Doing the measurement *after* hand-flattening them makes it
worthless. Schedule it before the §7.4 sweep touches comm/lander.

### 2. `UQM_WAIT_UNTIL` is the pacing bug waiting to recur

`UQM_WAIT_UNTIL` yields `UQM_PENDING`, so every poll iteration runs the full
input preamble and consumes a `PulsedInputState` edge. That is correct for
input-reading loops (`WaitForAnyButton`), but for a wait on anything else —
fade completion, stream drain, animation done, exactly what the comm/lander/
cutscene conversions will need — it eats keypress edges that the original
code would have delivered: a raw `while (!cond) TaskSwitch();` never ran
`UpdateInputState`, so the first sample *after* the wait still saw the edge
of a key pressed during it. This is the same divergence class as the §7a
pacing bug, one macro over.

It is currently latent — grep shows zero uses of `UQM_WAIT_UNTIL`/
`UQM_WAIT_WHILE` outside `coroutine.h` — which is precisely why now is the
time to fix the macro, before the sweep reaches for it. Suggestion: make the
default variant yield `UQM_PACING` and add an explicit
`UQM_WAIT_UNTIL_INPUT` for conditions that read input, or at minimum write
the rule into `coroutine.h`'s rule block alongside the others.

### 3. A `UQM_CALL`ed task's first frame runs on a stale input sample

`DoInput` ran the preamble before *every* `InputFunc` call, including the
first. `UQM_CALL` runs the sub-task's first frame immediately, on whatever
sample the parent last saw — which the parent may already have acted on. A
screen that read `PulsedInputState` before its first yield could double-act
on the parent's edge. Every conversion so far dodges this by shape: the entry
chunk draws, then the loop opens with `UQM_YIELD` before any input read
(`restart.c:161`). But that is a convention the notation does not enforce and
the rule block does not state. One sentence in `coroutine.h` — "yield before
the first `PulsedInputState` read, or flush input in the entry chunk" — turns
a silent hazard into a review checkbox. Cheap, and the §7.3 sweep is about to
convert ~30 screens whose shapes vary.

### 4. Make the DoInput undercount visible now, not later

The gate's known gap (an unconverted `DoInput` screen counts zero unless its
`InputFunc` sleeps) means "79 → 0" is not the finish line; the ~48 `DoInput`
call sites are co-equal remaining work. The doc defers folding them in "as
its own change with its own budget bump". Better: have the script print the
`DoInput` call-site count as a *second, ungated* number today. Zero cost, no
budget churn, and both the burn-down chart and the flip criterion ("both
numbers zero") become honest immediately. Gate it later if it ever moves the
wrong way.

### 5. Singleton contexts are accumulating an unasserted invariant

`battleCtx`, `starcon2MainCtx`, `StartGame`'s file-static context, and
`nestedPres[]` all assume one-at-a-time use, each justified individually
(the game thread's 1024-word stack is real), none checked. Reentry would be
silent state corruption. A file-static context is zero-initialized and both
`UQM_END` and `UQM_RETURN` reset `pc` to 0, so `assert (c->_coro.pc == 0)`
in each `_Init` is a free reentrancy guard. Worth adding while the set is
still four.

### 6. Settle the pause/exit question before the sweep bakes in more callers

`PauseGame`/`SleepGame`/`ConfirmExit` firing from inside `UpdateInputState`
— i.e., from the middle of `GameFrame_Begin` — is flagged in the doc as
"decide before §7.6", but it shapes the driver contract that every remaining
conversion builds against, so it is really "decide before the sweep gets
much further". The modal-state answer sketched in the doc is right; the
concrete version: `UpdateInputState` only *latches* a pause request into a
flag, and the drive loop (today `RunScreenTask`, post-flip `SDL_AppIterate`)
checks the flag between frames and runs the pause task as a modal that
suspends the game task. That also removes today's oddity of a task being
driven from inside another task's preamble.

### 7. Small items

- `StateDriver_Push` (`gameinp.c:397`): under `NDEBUG` the overflow assert
  compiles out and the push silently no-ops — then `DoInput`'s matching
  `StateDriver_Pop` pops someone else's frame. Never happens at depth 16 in
  practice, but the failure mode is stack corruption; make the release path
  fatal or track push success.
- `setupmenu.c`'s conversion churned 3,242 lines for a 2→1 site reduction
  (function merge + reindentation). Reviewability of the sweep matters;
  where a merge forces reindentation, note it in the commit message so a
  reviewer knows `git diff -w` tells the real story.
- `docs/unthread.md` §2 still describes the flip-era architecture as an
  explicit game-state stack (`PushGameState` per screen). The implementation
  has superseded that: converted screens nest as `UQM_CALL` chains under one
  root task, and `stateStack` only carries *unconverted* screens — it dies
  with `DoInput` at the flip. §2 should be updated to match, or a future
  contributor will look for a state stack that the end state does not have.
- Netplay is exempted and compiled out — correct for the wasm target, but
  worth stating in §7.5 that the exemption is a scope decision (netplay's
  frame-delay input model is thread-shaped and is Phase 5 work), so nobody
  mistakes it for an oversight.

## Remaining-work reality check

Of the 79 counted sites, roughly ten are in `libs/` (`dcqueue.c`'s condvar
pair, `tasklib`, `stream.c`, `sound.c`, `input.c`, `gfx_common.c`,
`vidplayer.c`) and mostly die at the flip or with already-planned audio work
rather than needing conversions. The real spine of what is left:

| Cluster | Sites | Character |
|---|---|---|
| `comm.c` + `starbas.c` + `melnorm.c` | 12 | talk animations; QA-heaviest; §9.5 decision pending |
| `planets/` (lander, solarsys, pstarmap, scan, …) | ~18 | lander sequences plus a spread of screens |
| supermelee residue (pickmele, melee, buildpick, cnctdlg) | 9 | mid-function sleeps, some netplay-adjacent |
| starbase cluster (starbase, shipyard, outfit, pickship) | 7 | deferred pending a save game — fair, but it gates on comm anyway |
| encount/gameopt/confirm/getchar/misc UI | ~12 | mechanical §7.3 sweep material |

Plus the ~48 `DoInput` call sites the gate does not yet show (finding 4).
At the observed velocity (27 sites across eight conversion commits, with the
per-class bugs already found and fenced), the mechanical majority looks like
what the plan says it is. The risk concentration is entirely in comm/lander —
which is why finding 1 is first.

## Bottom line

Keep going, in this order: (1) the Asyncify measurement, (2) the
`UQM_WAIT_UNTIL` fix and the first-read rule while both are still free,
(3) the second gate number, then resume the §7.3 sweep. The strategy needs
no revision — the plan's own hedge (§9.4 scoped bridge) just needs its
deciding measurement actually taken before the two files it exists for are
converted by hand.
