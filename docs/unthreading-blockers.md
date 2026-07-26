# What is left of the un-threading, and what blocks it

Companion to `unthread.md`, which is the map and the method. This is the
remainder, grouped by **what blocks each site** rather than by file, because
at this point the file a site lives in is rarely the interesting fact about
it.

State: **47 sites**, at `99b63c2d7` on `modernize/wasm` (from 106 at the start
of the sweep). Counted by `tools/unthread-burndown.sh`; the CI gate matches.

The short version: **roughly 20 of the 47 are gated on two structural changes
(`unthread.md` §7b), not on more conversion work**, and another ~7 are not
conversions at all — they are deleted at the flip. The genuinely
file-by-file remainder is small.

---

## 1. Indirect dispatch: the `GenerateFunctions` vtable — ~6 sites

| File | Sites | Reached through |
|---|---|---|
| `util.c` | 3 | `planets/report.c` (`MakeReport`, `DoDiscoveryReport`) ← `generateOrbital` |
| `planets/pl_stuff.c` | 1 | `ZoomInPlanetSphere` ← `LoadPlanet` ← `generateOrbital` |
| `planets/generate/genpet.c` | 1 | `ZapToUrquanEncounter` ← `pickupEnergy` |
| `planets/lander.c` | 1 | `KillLanderCrewSeq` ← `GenerateSol_pickupEnergy` |

The caller is a **function-pointer table entry**, not a named call site. A
leaf cannot become a task without changing the vtable signature and every
implementation — about 26 of them for `generateOrbital`.

The three `util.c` entries are worth calling out: `WaitForNoInput`,
`WaitForAnyButtonUntil` and `WaitForAnyButton` are *already tasks*. Only
their transitional wrappers survive, purely because `report.c` sits under
the vtable. Nothing about those waits needs converting.

**Unblocked by:** `unthread.md` §7b.2 (deferred-modal mechanism).

## 2. Indirect dispatch: comm's `RESPONSE_FUNC`s — 9 sites

| File | Sites |
|---|---|
| `comm.c` | 3 (`DoTalkSegue`, `runCommAnimFrame`) |
| `comm/starbas/starbas.c` | 4 |
| `comm/melnorm/melnorm.c` | 2 |

Same shape as §1 and larger: all of them are reached only through
`RESPONSE_FUNC`s and `AlienTalkSegue`, from roughly **29 call sites across 15
race comm files**. Converting `AlienTalkSegue` means converting that whole
surface.

This is the block `unthread.md` §7.4 calls the QA-heaviest work left, and it
is the largest single group remaining.

**Unblocked by:** §7b.2. Worth running the §9.5 Asyncify measurement *before*
committing to flatten it by hand — this is exactly the subsystem §9.4's
scoped bridge was recorded for.

## 3. Caller is a plain `InputFunc` — ~5 sites

| File | Sites | Blocking callers |
|---|---|---|
| `gameopt.c` | 2 | `encount.c:DoSelectAction`, `outfit.c:DoOutfit`, `shipyard.c:DoShipyard` |
| `confirm.c` | 2 | `DoPopup`'s dispatcher is a plain `DoInput`; `DoPopupWindow` also from `melee.c:connectionFeedback` |
| `menu.c` | 1 | `DoMenuChooser`: six of eight callers are tasks, `encount.c` and `gameopt.c` are not |

The screen is dispatched from someone else's `DoInput`, so it cannot
`UQM_CALL` anything, and its pacing has no task-owned loop to lift into.

**Unblocked by:** §7b.1. Once `DoInput` waits on `INPUT_STATE_DESC::NextFrameTime`,
each of these is a one-line edit, in any order, with no caller conversion.

## 4. Called from inside the per-frame preamble — 3 sites

`util.c` ×2 (`PauseGame`, `SleepGame`) and `uqmdebug.c` ×1 (`waitForKey` ←
`debugContexts` ← `debugKeyPressedSynchronous`), all invoked from
`UpdateInputState`.

This closes `unthread.md` §7a's long-standing open question, and the answer
is not what that section guessed. They **cannot** be driven as a modal state
by the main loop: `UpdateInputState` is the bulk of `GameFrame_Begin`, so
driving them per frame re-enters the preamble while `GamePaused` /
`GameActive` still asks to pause — unbounded recursion. All three already
sidestep the preamble deliberately: `PauseGame_Frame` polls
`ImmediateInputState` and `BeginInputFrame` itself, `SleepGame_Frame` waits on
`GameActive`, and `DoConfirmExit` clears `ExitRequested`/`GamePaused` every
iteration (see its "Forbid recursive calls" comment).

**Unblocked by:** hoisting the `GamePaused` / `GameActive` / `ExitRequested`
checks *out* of `UpdateInputState` and into the driver loop. A bridge swap
cannot work. Small change, now well understood — good candidate to do early.

## 5. Transitional wrappers waiting on one caller — ~12 sites

Each is a single `RunScreenTask` or `UQM_RUN_BLOCKING` that dissolves when its
last non-task caller converts. No design work, just ordering.

| File | Waiting on |
|---|---|
| `battle.c` | `melee.c:StartMelee` (**not** `starcon.c`, which already uses `UQM_CALL`) |
| `fmv.c` | `DoShipSpin` ← `shipyard.c`, `buildpick.c` |
| `getchar.c` | five callers, all `InputFunc` or widget-vtable callbacks |
| `encount.c` | `comm.c:InitCommunication` (26 callers, 18 under the vtable) |
| `ship.c` | `tactrans.c:ship_death`, inside `RedrawQueue`'s element-death path |
| `melee.c` ×3 | `StartMelee`'s two fades; `DoConfirmSettings`'s mid-function netplay wait |
| `shipyard.c` | `ShowCombatShip`'s door animation — mid-function, ~300 bytes of live locals |
| `cnctdlg.c`, `intel.c` | one each |

## 6. Deleted at the flip, not converted — ~7 sites

`libs/graphics/dcqueue.c` ×2 (the `TFB_WaitForSpace` condvar, meaningless once
producer and consumer are the same thread), `libs/task/tasklib.c`,
`libs/video/vidplayer.c`, and `starcon.c` ×4 (`BackgroundInitKernel`,
`SignalStopMainThread`, and the root bridge itself, which `SDL_AppIterate`
replaces per §7.6).

Plus `planets/solarsys.c` ×1, which is **dead code** inside a never-compiled
`#ifdef SMOOTH_SYSTEM_ZOOM`. The gate counts it; nothing else does.

---

## The number still lies, in one specific way

**Seven `DoInput` call sites remain, and the gate does not count them** —
`gameinp.c` is wholly exempt, so `DoInput`'s own frame boundary is invisible.
Zero on the burn-down will *not* mean the thread can go.

This is `unthread.md` §7.5's recorded undercount, and it is why §7b.1 insists
the `INPUT_STATE_DESC` change lands in the same commit as a gate that counts
`DoInput` call sites. Expect the number to jump when that happens; that is
the metric becoming honest, not a regression.

## Recommended order

1. **§7b.1** — `NextFrameTime` in `INPUT_STATE_DESC`, `DoInput` does the
   waiting, gate fixed in the same commit. Cheap, clears category 3, and
   makes category 5 easier by removing the top-down ordering constraint that
   has driven every deferral so far.
2. **The `UpdateInputState` hoist** — category 4. Small, self-contained, and
   the design question is now settled.
3. **Category 5** — mechanical once 1 and 2 land.
4. **§9.5's Asyncify measurement** — before deciding how to attack categories
   1 and 2. If the scoped bridge (§9.4) is viable for comm and lander, that is
   ~15 of the remaining sites that never need hand-flattening at all.
5. **§7b.2** — the deferred-modal mechanism, if the measurement says flatten.

## How this list was built

Four independent conversion passes this session converged on the same two
walls from different directions — the vtable and "caller is a plain
`InputFunc`". That convergence, more than the count, is the reason §7b exists
and the reason the recommended order starts with structure rather than with
more files.
