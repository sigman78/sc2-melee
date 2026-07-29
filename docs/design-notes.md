# Design notes and the divergence ledger

Two registers in one file. **Decisions** hold the rationale that used to live
in long comments (docs/cpp-conventions.md rule 10: source keeps the claim and
a pointer here). **Divergences** is the ledger of every place the rewrite
deliberately departs from the C — the plan's policy is that a difference
discovered later must be attributable, so a departure without an entry here
is a bug by definition. Review-001 (`review-001.md`) holds the full audit
this file distills.

## Decisions

**D1 — The step loop walks the live list.** Both passes follow the real
list, as `PreProcessQueue`/`PostProcessQueue` do: a tail spawn is reached by
the pass that caused it, so a weapon moves on the frame it is fired and
one-frame elements act on their only frame. Committed elements are protected
from the whole-list catch-up walks by `PostProcessed` (the C's POST_PROCESS,
process.c:859); without it they integrate twice per frame. Snapshot passes
were tried first and cost every projectile its first frame of flight.

**D2 — Collision is masks, not canvases.** intersec.c reports "no hit"
without a graphics context (intersec.c:245), and weapon.c:274-286
rejection-loops on it — a naive headless build hangs. The sim owns 1-bit
opacity masks with hotspots; the renderer never participates in collision.

**D3 — Impact placement interpolates.** The C stores
`DISPLAY_TO_WORLD(SavePt)` (process.c:578-595), snapping every impact to a
4-world-unit grid; the rewrite rewinds the original world-space motion to
the impact time. Snapping pulled contacting ships back together each frame
and they stuck. (Divergence V2.)

**D4 — Integration is unwrapped; the wrap happens at commit.** The C only
wraps positions in the display update (process.c:899-916). Wrapping during
integration turned a seam crossing into a full-arena sweep and manufactured
phantom hits. Cost, same as the C's: a genuine seam collision lands a frame
late.

**D5 — Sound and spawn/collision data are outputs of step().**
`CollisionEvent`/`SpawnEvent` are observational: presentation reads them,
nothing feeds them back. Scanning element flags after the step was tried and
silently depended on a step-loop bug.

**D6 — One camera, one zoom.** The C's `optMeleeScale` forks the camera and
the sprite-LOD path; both behaviours are kept but both now emit a single
1/256ths zoom that everything downstream reads. Collision masks come from
the 1:1 art and do not change with zoom (divergence V1).

**D7 — Input accumulates.** `held` plus a sticky `pressed` bit consumed once
per sim step, so a tap shorter than a 42 ms frame lands exactly once. The C
polls levels at frame rate and drops sub-frame taps (divergence V6).

**D8 — The entity list is an ordered arena.** Stable generation-checked ids
over chunked storage (addresses never move — ships hold pointers to
themselves across spawns), plus an explicit order with head/tail insertion,
because traversal order is observable gameplay. See
docs/sim-architecture.md.

**D9 — TFB_Random is reproduced bit for bit,** including the uint32 wrap
that makes it not-quite-Park-Miller; `std::minstd_rand` agrees for several
draws from many seeds and then silently diverges. Golden vectors are
compile-time asserts in Random.hpp.

**D10 — Content is addressed by resource id, not path.** `uqm.rmp` is the
only link between a name and a file: directory and resource names disagree
by design (`blackur/` loads `comm.kohrah.*`), and the indirection is what a
content override replaces. The melee app hardcoded paths early and worked —
right up until it needed the indirection back.

**D11 — Audio needs no thread of ours.** `SDL_AudioStream` is fed from any
thread and owns its own mixing, satisfying "the device callback must never
see game state" with no ring buffer and no atomic anywhere in `src/`.

**D12 — One voice per distinct sound.** Loudness is governed by voice
count, not gain: a per-frame effect (the flame) stacks additively across
round-robin streams, so a repeat restarts the stream already playing that
sound — the C's per-source channel replacing its old sound (sound.c). The
flat 0.35 effect gain is an honest level only because of this.

**D13 — Single-threaded is a WASM deployment requirement.** A second thread
forces SDL_PTHREADS under Emscripten, which forces SharedArrayBuffer, which
forces COOP/COEP headers before the page loads — a cost M1's plain-URL
deploy cannot pay.

**D14 — The fixed-step accumulator advances its deadline by a period, never
resets it to now.** The naive form silently runs a 24 Hz battle at 20 Hz on
a 60 Hz display loop; `tests/engine_test.cpp` asserts both rates forever.

**D15 — EntityRef is a debug-checked borrow.** Stable addresses let a hook
hold a self-pointer across a spawn, but removal leaves such a pointer at a
default-constructed T — silently wrong, not UB. Debug builds assert
liveness on every dereference with an epoch delta naming the mutating call;
release builds are a bare pointer.

**D16 — The starfield is a plain pan.** Three planes scroll at 1/2^plane of
the camera (galaxy.c:37-44, 405-407) without the C's incremental y-sorted
bookkeeping; zoom deliberately never enters star positions — stars are at
infinity, and a drifting zoom divisor shimmered the whole field.

**D17 — Content is found from the working directory and the executable's
directory, walking upward from both.** Neither alone is reliable (Explorer
cwd, build-tree cwd). A directory counts only once `uqm.rmp` is in it, so
an empty `sc2/content` cannot impersonate the real one.

**D18 — SpeedState is derived, not flagged.** Computed from |v| against
max thrust instead of the C's hand-patched flags (chmmr.c:398-409,
druuge.c:266, mmrnmhrm.c:436-450); `applyImpulse` resets it to Normal
unconditionally, which is collide.c:104-110's own behaviour.

**D19 — Thrust takes facing and speed state as arguments** (engine
primitive #1), not from STARSHIP globals — deleting the save/overwrite/
restore dance Supox's omni-thrust needed (supox.c:242-271).

**D20 — Weapon spawns are pure descriptors** (engine primitive #5). The
C's AI lookahead fires weapons for real on copied elements
(cyborg.c:339-410) and Umgah's init leaks a heap write through the shared
RaceDescPtr (umgah.c:330-341, with orz.c:249-253 as the compensating
hack). A spawn function takes a const view and returns values; the write
path does not exist.

**D21 — ElementKind is a real type tag,** not a frame-pointer comparison
(cyborg.c:1222-1227) or a cross-ship header include (shofixti.c:251-253).

**D22 — The world coordinate space is fixed at compile time.** Plan open
question 4, settled: `ScreenWidth/Height`'s only assignment is
sdl2_pure.c:312-313; `--res` moves the window, never the arena.

**D23 — Velocity's packed sign-in-byte-pair encoding is kept bit-exact.**
The carry, truncation and sign handling interact; a tidier signed
fixed-point pair would change trajectories.

**D24 — ARCTAN's sentinel outlives the Angle type.**
`Velocity::travelAngle()` stays a plain int because 64 means "no
direction": unequal to every real angle in comparisons, folded to 0 by
table indexing when it reaches trig — exactly what `Angle`'s wrapping
constructor does, and what an optional would break in collide()'s
stuck-pair branch.

**D25 — The warp-in trail's direction and shape both carry meaning.**
Inward-marching, ship-shaped images (tactrans.c:938-950): a point stack is
invisible against the hull, outward flight reads as the ship coming apart.

## Divergences (deliberate, recorded)

**V1 — Collision masks are zoom-independent.** The C tests whatever frame is
displayed, so hitboxes shrink as the camera pulls out and distant ships pass
through each other. The sim's silhouette is fixed at 1:1. Changes collisions
at range.

**V2 — Impact points interpolate** instead of snapping to the display grid
(see D3). Sub-pixel differences in every resolved contact.

**V3 — Point defence applies damage directly** and spawns a decorative beam.
The C spawns a real laser element that can be intercepted en route and
leaves a small blast. Guaranteed hit on the chosen target here; no
incidental interceptions.

**V4 — Point defence skips gravity masses.** The C pays energy to shoot the
planet, which absorbs the hit (do_damage exempts it, misc.c:214). Verified
against human.c; kept because it reads as a malfunction.

**V5 — Point defence works across the arena seam.** The C computes raw
deltas (human.c:208-217) and is blind across the wrap.

**V6 — Input is accumulated, not polled** (see D7). A feel change; the melee
baseline is established with it in place.

**V7 — Ships spawn with a minimum separation.** The C will happily start a
melee with the ships touching (ship.c:473-481 has no separation test);
placeShipAtRandom adds a floor, relaxed if placement keeps failing.

**V8 — Presentation draws from its own RNG stream.** Consequence accepted in
the plan: post-battle RNG position is not reproducible against the old
build.

**V9 — Blast lifetimes are a constant 5.** The C derives them from sprite
frame counts (weapon.c:215-236): 9 for the nuke, 2 for the flame. Visual
only; revisit when blast art is data-driven.

**V10 — The ion trail offsets by half the mask height,** where the C uses
hotspot-to-bottom-edge of the frame (tactrans.c:809-811). Equal for centred
hotspots; visual only.

**V11 — TrackShip re-scans every call.** The C locks `hTarget` and
re-evaluates only it until the lock breaks (weapon.c:328-335). Identical in
a 1v1; diverges once a third ship exists — scheduled with M2.

**V12 — Starfield density is fixed, not zoom-varying.** The C sizes each
star plane's space to the camera reduction (galaxy.c:248-259), so on-screen
count runs from ~3 stars at 1:1 to all 180 fully zoomed out. A
zoom-independent field cannot have both ends; a four-screen tile (~45 in
view) splits the difference.
