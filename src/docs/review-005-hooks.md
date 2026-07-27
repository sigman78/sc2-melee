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

## 2. The stages, as executed

Mid-review, SiGMan added a directive that became the centerpiece:
ordering should be *declarable* — a fixed list of strata — not implicit
insertion order. The stage list grew accordingly.

| Stage | What | Landed as |
| --- | --- | --- |
| Y1 | `Guided{trackWait, maxSpeed, thrustScale, clock}` on guided shots; `Spin{backwards, period, countdown}` on asteroids | done — RNG draw order unchanged; the repeatability pin now compares the Spin component itself, after the old field comparison was caught passing vacuously (0 == 0 on a retired field) |
| Y2 | **Declared ordering**: the spine segmented by `Layer{Background, Field, Ordnance}`; `spawn(Layer, Element)` is the one spawn API; spawnFront/spawnBack/insertAfter deleted | done, bit-green — for everything with gameplay effect the layered chain equals the old chain; only decoration-vs-decoration order changed (FIFO within Background, was LIFO by head-insert), and decorations are hook-free and non-solid. The Pkunk phoenix's future position is a declared layer, not an insertion trick |
| Y3 | `WarpingIn` and `Exploding`: hook self-mutation becomes component presence; shipPreProcess dispatches | done — and the explosion path turned out to have no test at all; it has one now. A placement bug (the dead-hull gate before the first-frame crew fill) was caught by the suite as a missile that never fired |
| Y4 | The ship phases as named systems (regenEnergy, turnShip, applyThrustInput, fireWeapon, gateSpecial), called in pinned order | done — pure extraction, by subagent from the authored spec |
| Y5 | `Input` component (the app→sim write gets a type boundary); `Battle::attach/find/has/detach<T>` + `eachElement`, `registry()` private; the two order-free boolean scans become view-based | done, by the same subagent; `attach<T>` returns `decltype(auto)` because entt's emplace returns void for empty tag types |

## 2a. Verdict

- The hooks question has a three-part answer, now all in code: behavior
  *state* belongs in components (Guided, Spin, Cloak); *optional phases*
  are component presence (WarpingIn, Exploding — the C's self-modifying
  hook edge is closed); the *slots* survive only as how the walk finds a
  behavior, and their values must be thin system functions over their
  components.
- Ordering is now declared. The strata were always there in the C's
  insertion tricks; naming them cost one enum and made the sim's frame
  structure legible at the spawn site. The catch-up semantics survived
  unchanged because "ordnance after the field" is what tail insertion
  was *for*.
- `Battle`'s registry is no longer reachable from outside; the app works
  through the typed surface, which turns review-004's layering
  convention into a compile error.
- Cost honestly paid: the fire block spawns-then-attaches (a tag cannot
  ride the Element value into spawn), and one real bug was written and
  caught during Y3 — the suite remains the reason stages like these are
  cheap.

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
