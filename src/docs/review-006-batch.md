# Review 006 — the batch frame: systems over views, order as data

SiGMan's directive: dig further into ECS territory. One frame of reaction
latency is acceptable (with a newborn second-pass or a 48 Hz sim held in
reserve), so the pure batch-systems shape is on the table: read/write
declared per view, systems communicating through commands and effect
components, the pre/post walks becoming a sequenced pipeline. This review
is the design and the staged route; stages execute like review-004/005's,
each suite-checked and committed.

## 1. The frame as a pipeline

The two live walks become a fixed, declared system list. Single-threaded
and sequenced: later systems see earlier writes, and the sequence itself
is the ordering contract (double-buffering becomes necessary only with
parallel execution, which nothing here needs).

| # | System | Reads | Writes | Emits |
| --- | --- | --- | --- | --- |
| 1 | AgeAndReap | lifeSpan | Disappearing | death commands |
| 2 | EnergyRegen | ShipState | ShipState | |
| 3 | ShipMachines (cloak…) | Input, ShipState | Cloak, ShipState | |
| 4 | Turn | Input, ShipState | facing, mask, turnWait | |
| 5 | Thrust | Input, ShipState | velocity, thrustWait | exhaust spawn |
| 6 | GuidedSteer | Guided, targets | facing, velocity | |
| 7 | Animate | Flame-like | colorCycle, mask | |
| 8 | Integrate | velocity (−IgnoreVelocity) | next | |
| 9 | Collide (algorithm-system) | collidables | Collided, next, velocity | CollisionEvent, DamageIncoming |
| 10 | Fire / SpecialGate | Input, ShipState | counters, energy | weapon spawns, beam + damage |
| 11 | **sync point** | | spawns/damage/deaths applied | SpawnEvent |
| 12 | Commit | next (−BeamGeometry) | current, wrap | |

Read/write discipline: a system's view is `view<const A, const B, W>` —
const-ness is the read-only declaration and the compiler enforces it; the
table above is the documentation format every system keeps.

## 2. Communication rules

- **Effect components** when the target exists and effects stack:
  `DamageIncoming{amount, from}` accumulated on the victim by collide, PD
  and beams; consumed by one apply pass at the sync point, then cleared.
  Stacking becomes explicit (sum, one death check) instead of accidental
  (whichever hook ran first).
- **Commands** when no target exists yet or the effect is structural:
  spawn, destroy, attach/detach. A typed vector filled in pipeline order;
  emission order is deterministic because the pipeline is.
- Events (CollisionEvent, SpawnEvent) stay observational outputs.

Two refinements settled at Z4 planning:

1. **Snapshot reads for cross-entity aiming.** trackShip and gravity read
   `current` unconditionally — the frame-start position. The C's
   read-`next`-if-PreProcessed dance existed because the walk moved half
   the world before the other half looked; with steering before Integrate
   there is one consistent snapshot and the flag dance dies.
2. **DamageIncoming is for crewed hulls only.** Munition-vs-munition
   hit-point exchange (piercing — a pinned protocol) resolves pair-locally
   inside the collide system, where the pair order makes it deterministic;
   ship crew damage stacks in DamageIncoming and applies once at the sync
   point, one death check per frame. Splitting by target keeps piercing
   exact and makes multi-source ship damage explicit.

## 3. Order as data, completed

With batch passes the OrderLink spine retires. What survives is `Layer`
plus a monotonic `Seq` (u64 spawn counter, one component): targeting
tie-breaks, PD's pay-once volley and collision pair order sort or scan by
`(Layer, Seq)` — a declared comparator where the old code had a hidden
list position. Draw sorts its visual set by the same key. Most systems
need no order at all, and say so by not sorting.

## 4. The latency budget

Spawns materialize at the sync point and act next frame — the accepted
cost. Two flagged exceptions where the C's feel may demand the newborn
second-pass (re-run 6/8/9 over entities born this frame, before Commit):

1. PD burns a shot the frame it appears (pinned, balance-relevant).
2. The muzzle-velocity backoff assumes same-frame integration.

Start with plain one-frame latency; the harness (below) decides whether
those two need the pass. 48 Hz stays in reserve: every wait/life constant
is frame-denominated, so it means doubling every number in every spec.

## 5. The oracle changes at system 9

Systems 1–8 are per-entity independent: batching them is syntax, and the
existing bit-exact suite must stay green through their conversion.
Bit-exactness ends at Collide — the live walk interleaves each entity's
motion with its collision tests, and "integrate all, then collide all"
changes which positions pairs see. Before that stage lands, a
**replay-similarity harness** must exist: seed sweeps on the current sim
recording winner, damage timeline and entity counts; converted stages
must match statistically. The bit suite remains for what stays
bit-identical; the harness carries what does not.

## 6. The stages

| Stage | What | Proof |
| --- | --- | --- |
| Z1 | Ship.cpp split by concern: ShipSystems (generic frame), Targeting (trackShip), ships/Human, ships/Ilwrath; nukePreProcess renamed guidedShotPreProcess (it is the generic guided-shot system) | suite green; pure file movement |
| Z2 | The replay net: 32 seeded battles, per-frame FNV-64 digests, 64-frame divergence checkpoints, --trace for two-build diffs | **done** — demoted from approval oracle to regression instrument on SiGMan's challenge: --compare (bit-exact) is the ctest gate, re-recorded only at intentional semantic changes; --similar gates nothing. Stage find: bare spec literals carry empty weapon masks, so harness shots collided with nothing until the specs were materialized test-side |
| Z3 | `Seq` component; EnergyRegen becomes the first true batch pass — **done**, proven the hard way: all 32 replay battles bit-identical against the unchanged baseline | **bit-green required** — and the corrected claim is that regen is nearly the *only* pass that can be: aging, animation, steering and integration are all observable through walk position (batch-aging shifts death frames, batch-animate re-masks the flame mid-walk, batch-integrate changes what collision pairs see). The §1 table stands as the destination; the bit-green on-ramp is far narrower than first drafted |
| Z4 | The semantic flip, in one harness-verified stage: aging, animate, steer, integrate as batch passes; effect components + command buffer + the sync point; Fire/SpecialGate emit instead of spawning inline; one-frame spawn latency arrives | harness-green; the bit suite keeps whatever still passes, divergences documented per test |
| Z5 | Collide as the pair-worklist algorithm-system with (Layer, Seq) pair order | harness-green |
| Z6 | Spine retirement: OrderLink deleted, draw sorts by (Layer, Seq); the verdict, measurements, and sim-architecture.md amended again | the report card |

## 7. Declined up front

- Parallel system execution: nothing at this scale earns it, and it would
  force per-system RNG streams, breaking the C oracle for zero payoff.
- Batch dispatch of ship specials by component chains in the step loop:
  specials stay spec-driven functions (review-002 §1's argument does not
  weaken); what changes is *when* they run (pipeline slot), not *how they
  are found*.
