# The Asyncify measurement (unthread.md §9.5)

A one-day experiment that produces the three numbers deciding §9.4: whether
`comm.c` and `lander.c` (six blocking sites each, the QA-heaviest flattenings
left) get converted by hand, or hosted in a scoped Asyncify bridge. Everything
here stays behind a CMake option that defaults OFF; nothing lands in the
shipping build, and none of it requires the fiber backend to exist — that is
the *follow-on* investment, made only if these numbers come back tolerable.

Starting point, already in the tree: the `wasm` preset builds and runs in the
browser today (pthreads + SharedArrayBuffer, `-sASYNCIFY=0` —
`CMakeLists.txt:123`), emsdk is found by `cmake/uqm.toolchain.wasm.cmake`,
and `python tools/serve-wasm.py` serves with the COOP/COEP headers the
pthread build needs.

## The one non-obvious prerequisite

**Flipping `-sASYNCIFY=1` alone measures nothing.** Asyncify only instruments
functions that can *transitively reach* an unwinding import such as
`emscripten_sleep()`. Nothing in the tree calls one, so the instrumented set
would be empty and the binary nearly unchanged.

To measure the real cost, give the static analysis the same unwind root the
§9.4 bridge would have: make the blocking primitives in
`sc2/src/libs/threads/sdl/sdlthreads.c` reach `emscripten_sleep()`. Runtime
behavior stays byte-identical under the pthread build by hiding the call
behind an opaque flag that is never true — the analysis instruments
everything that *could* reach the call, whether or not it ever executes:

```c
#ifdef __EMSCRIPTEN__
#include <emscripten.h>
/* Never true at runtime; volatile keeps the optimizer from proving it.
 * Asyncify's static analysis instruments every function that can
 * transitively reach emscripten_sleep, executed or not -- which is
 * exactly the call graph the §9.4 bridge would suspend through. */
static volatile int asyncifyProbeArmed = 0;
#define ASYNCIFY_PROBE() \
		do { if (asyncifyProbeArmed) emscripten_sleep (0); } while (0)
#else
#define ASYNCIFY_PROBE() ((void) 0)
#endif
```

Drop `ASYNCIFY_PROBE ()` at the top of `SleepThread_SDL`,
`SleepThreadUntil_SDL`, `TaskSwitch_SDL`, and `WaitCondVar_SDL` — the four
suspension points a cooperative backend would actually yield at.

## Build variants

Add one option to `CMakeLists.txt` and make the existing `-sASYNCIFY=0` in
the `if(EMSCRIPTEN)` flags block conditional on it:

```cmake
option(UQM_ASYNCIFY_MEASURE
		"Instrument the wasm build with Asyncify (docs/asynctify-test.md)" OFF)
```

| Variant | Extra link flags | What it answers |
|---|---|---|
| A: baseline | *(none — today's build)* | reference size and frame cost |
| B: whole-program | `-sASYNCIFY=1 -sASYNCIFY_STACK_SIZE=65536` | upper-bound size and speed cost |
| C: advise | B + `-sASYNCIFY_ADVISE=1` | how big the instrumented set is, and *why* |
| D: scoped estimate | B + `-sASYNCIFY_IGNORE_INDIRECT=1` | lower-bound size for a §9.4-style scoped bridge |
| E: JSPI (optional) | `-sJSPI=1` instead of Asyncify | does the zero-instrumentation path launch in Chrome |

Configure/build as usual (`cmake --preset wasm -DUQM_ASYNCIFY_MEASURE=ON`,
`cmake --build --preset wasm`); the compile definition also gates the probe
macro and the frame timer below.

Notes per variant:

- **C** prints its report at link time — capture the build log. Count the
  instrumented functions (`grep -c` the advise lines) and note the recurring
  "because it calls X indirectly" chains. UQM's `InputFunc`-pointer style is
  the wildcard here: Asyncify treats every indirect call as potentially
  reaching every address-taken function of matching type, which can poison
  nearly the whole binary. If C already shows the set is most of the game,
  the scoped bridge is whole-program in disguise and the experiment is over.
- **D** exists for its *size number only*. `ASYNCIFY_IGNORE_INDIRECT` is not
  runtime-correct for UQM (a suspend crossing an uninstrumented frame is a
  runtime error); a real scoped bridge would need a hand-curated
  `ASYNCIFY_ADD` list. D brackets the cost from below, B from above.
- **E** costs one rebuild. JSPI has no instrumentation, so size/speed ≈
  baseline by construction; the only datum is whether it starts in current
  Chrome. The real JSPI blocker is the three-browser matrix (unthread.md
  §9.2) — that is a docs check, not a build.

## The measurements

### 1. Artifact size (variants A, B, D)

Raw and compressed — compressed is what a player downloads:

```sh
ls -l build/wasm/uqm.wasm
gzip -9 -c build/wasm/uqm.wasm | wc -c
```

### 2. Game-thread CPU per frame (A vs B)

The subtlety: instrumented code pays its entry/exit overhead on the *game*
thread, whose pacing loops absorb slowdown silently — battle at 24 Hz has a
~35 ms budget, so wall-clock FPS will not move until the overhead is
catastrophic. Do not measure FPS; measure work per game frame.

Precise method — under `UQM_ASYNCIFY_MEASURE`, time the root task's frame in
`RunScreenTask` (`sc2/src/uqm/gameinp.c`): wrap the `frame (context)` call in
`emscripten_get_now ()` pairs, accumulate, and `printf` mean and p95 every
few seconds (the shell HTML's `note` log picks stdout up). That times exactly
the instrumented call graph, once per game frame, on the thread that runs it.

Crude cross-check — Chrome DevTools Performance panel: record ~15 s of the
same scene in A and B and compare the game worker thread's total CPU time.
Zero code, good enough to confirm the precise number's direction.

**Scene:** Super Melee battle — reachable from the main menu without a save,
the heaviest sustained loop, and squarely inside the instrumented set. Pick
the same two teams both runs. Two traps from earlier sessions (unthread.md
§7a): the idle main menu drops into splash/credits after 20 s (no music
addon), and injected keys need real scan codes plus a ~150 ms hold if the
runs are scripted rather than played.

## Deliverable

Fill this in and append it to unthread.md §9.5:

| | A: baseline | B: whole-program | D: scoped floor |
|---|---|---|---|
| `uqm.wasm` raw | | | |
| `uqm.wasm` gzip -9 | | | |
| instrumented funcs (from C) | 0 | | |
| game-frame CPU mean / p95 | | | n/a |

Suggested reading of the numbers (owner call, these are defaults):

- **Bridge is viable** if the D→B size range stays under ~10–15 % compressed
  *and* B's game-frame CPU is under ~1.5× baseline *and* C's set looks
  containable to a curated list. Then §9.4 (fibers + `ASYNCIFY_ONLY` for
  comm/lander) is worth its days of follow-on work.
- **Bridge is dead** if C shows near-total poisoning through indirect calls
  or B's overhead is large. Then comm/lander get flattened by hand like
  everything else, and §9.5 gets one line saying so with the numbers
  attached — which is the entire point: either outcome, measured, beats the
  prior either way.

## Effort and cleanup

Roughly a day: ~1 h for the option, flags, and probe macro; ~2 h for the
frame timer and baseline numbers; the rest is rebuilds and two browser
sessions. All of it sits behind `UQM_ASYNCIFY_MEASURE=OFF`; the probe adds no
blocking sites, so the burn-down gate is unaffected. Keep the branch around
until the §9.5 verdict is written down, then delete everything except the
numbers.
