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
`DISPLAY_TO_WORLD(SavePt)`, snapping every impact to a 4-world-unit grid;
the rewrite rewinds the original world-space motion to the impact time.
Snapping pulled contacting ships back together each frame and they stuck.
(Divergence V2.)

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
