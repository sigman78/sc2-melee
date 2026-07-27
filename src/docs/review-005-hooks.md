# Review 005 — hooks meet components, and the ship phases get names

SiGMan's questions: can the hook approach convert to components/systems,
and what about the shipPreProcess/shipPostProcess monoliths? Answered by
executing, on top of review-004's registry.

## 1. The answer's shape

A hook slot holds two things at once: *which* behavior an entity has, and
*where in the walk* it runs. The conversion splits them:

- **Behavior state and parameters become components.** The nuke's
  tracking clock lives in a repurposed `Element::turnWait`; the
  asteroid's spin is bit-packed into `thrustWait` (Field.cpp) — the
  exact "field repurposing" smell review-002 censused in the C, alive in
  our own tree. `Guided` and `Spin` end it, and they are the census's
  first two library components landed.
- **Optional behavior becomes component *presence*.** The C mutates
  per-instance hooks at runtime (chmmr.c:773, pkunk.c:282), which
  review-002 called the model's acknowledged edge. Our own tree does it
  twice: shipTransition swaps itself to shipPreProcess on arrival, and
  startShipExplosion swaps in explosionPreProcess, which then nulls
  itself. Both become components — `WarpingIn`, `Exploding` — whose
  presence *is* the behavior and whose removal *is* the hook swap. The
  edge stops being an edge.
- **The slots stay, as walk-order dispatch points.** Hooks run at their
  entity's position in the live walk, interleaved — a nuke steers at its
  own slot, after some targets have moved and before others. Batch
  systems ("for each Guided: steer") would change that interleaving,
  which is gameplay; a component-dispatch if-chain in the step loop would
  re-centralize every mechanic into the loop, which review-002 §1
  rejected for good reason. So: **a hook function becomes a thin system
  function over its component(s); the slot is just how the walk finds
  it.** That is the criterion a slot value must now meet.
- **The phase monoliths get names.** shipPreProcess/shipPostProcess are
  review-002 §2's latent-system table in two 100-line bodies. They become
  short sequences of named system functions (energy clock, turning,
  thrust, weapon fire, special gate) called in the pinned order — the
  reuse surface every M2 ship composes from.

## 2. The stages

| Stage | What | Proof |
| --- | --- | --- |
| Y1 | `Guided{trackWait, maxSpeed, thrustScale, clock}` on guided shots; `Spin{backwards, period, countdown}` on asteroids; the repurposed Element fields stop being repurposed | suite green; the asteroid RNG-repeatability pin (seven draws in order) unchanged |
| Y2 | `WarpingIn` and `Exploding`: hook self-mutation becomes component presence; shipPreProcess owns the delegation | suite green; the warp-in protocol pins (shadow count, monotone closing, NonSolid window) unchanged; the old preProcess-identity pin still holds trivially and gains a component assertion |
| Y3 | The ship phases as named systems, called in pinned order; hooks become thin | suite green — pure extraction, exact-order pins are the proof |
| Y4 | `Input` component (the app→sim write gets a type boundary); `Battle::attach/find/has/detach<T>` replace the raw `registry()` accessor, closing review-004's convention hole; the two order-free boolean scans become views | suite green + driven run |
| Y5 | This review's verdict; sim-architecture.md's hooks paragraph amended | the record |

## 3. What is deliberately not attempted

- No batch systems over behavior components — interleaving is gameplay
  (above).
- No component-dispatch chain in the step loop — the four slots remain
  the generic mechanism, filled from specs.
- `onDeath`/`onCollision` slot values (asteroidDeath, weaponCollision,
  flameCollision) stay free functions: their per-instance state is
  already component-shaped or absent, and the flame's linger is one
  wrapped call — converting them adds indirection, not structure. If an
  M2 mechanic gives one of them state, it gets a component then.
