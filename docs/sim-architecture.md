# Simulation architecture — entities, components, and what kind of ECS this is

Settled after review 001 and the first faithfulness pass, when the shape of
the C's per-ship customization — 25 ships, each with its own special, weapon
quirks and hooks — made the question unavoidable. The conclusion: **ECS wins
as a composition model, and loses as a storage model.** This document is the
boundary between the two, so the boundary survives contact with M2.

## The two claims inside "ECS"

**Composition** — an entity is an id; capability comes from typed components
attached to it; a ship is a declarative list of components plus parameters.
The C's own census supports this: `ships/` decomposes into ~15 reusable
components, ~19 one-offs and 27 tactics classes (game-rewrite-plan.md,
§Ship model). Every pressure the user-visible customization exerts — specials,
charge weapons, limpet stacks, tethered satellites — is composition pressure.
**Adopted.**

**Storage** — amended on the rewrite/ecs branch by review-004's
experiment. Adopted **as EnTT sparse-set pools, and only that**: an
`entt::registry` stores components keyed by entity, and an explicitly-kept
`OrderLink` spine owns traversal order. The two falsifiable reasons the
original decision rested on, re-examined against the executed code:

1. There is no performance problem to solve — still true, and nothing in
   the adoption was done for speed. Pools were adopted for the composition
   ergonomics (a component is a type and an `emplace`, not a hand-rolled
   sidecar), not for cache behaviour.
2. **Traversal order is gameplay** — held completely. The order never
   entered the library: the C's `disp_q` survives as the OrderLink
   component plus head/tail in Battle, and every ordered pass walks it.
   A sparse-set registry coexists with an owned spine precisely because it
   has no opinions about order. What this reason actually rejects —
   archetype/SoA storage that reorders entities by composition — remains
   rejected, exactly as before.

The costs paid for the pools are recorded in review-004's friction ledger
(3-4× per-TU compile time in sim/, the EntityRef diagnostic lost, the
kNoEntity/in_place_delete/spawn-then-tag disciplines). If a profile ever
contradicts reason 1 in the other direction, this file is still where the
argument gets reopened — not a refactor that quietly assumes it.

## Vocabulary

| Term | What it is here | What it is in the C |
| --- | --- | --- |
| **Entity** | `EntityId` = `entt::entity` — a versioned id; order lives in the `OrderLink` spine, not the storage | `HELEMENT` in `disp_q` |
| **Component** | A type in the registry, `emplace`d per entity (`ShipState`, `WeaponGuidance`, `Cloak`, tags, the app's `Visual`); `Element` keeps the universal motion/protocol/collision core | fields smuggled into `ELEMENT` or `STARSHIP` |
| **Behavior slot** | The four phase hooks — pre, post, collision, death — filled from the component library, per entity | per-instance function pointers |
| **System** | A cross-entity pass with its own state: gravity, camera, sound | special-cased calls inside the queue walk |
| **Event** | Observational output of `step()` — `CollisionEvent`, `SpawnEvent`. Never an input; nothing reads events back into the sim | draw/sound calls made mid-step |
| **Spec** | Immutable declarative description — `ShipSpec{thrust, weapon, special, ai}` built from `WeaponSpec`/`SpecialSpec` values. In code today, the TOML of the plan tomorrow; same shape either way | `RACE_DESC` plus per-ship `#define`s |

Hooks are deliberately NOT an event bus. Every ship in `ships/` was tuned
against direct per-entity dispatch, and M2's strategy (port `cyborg.c`
against a stable reference frame) needs that dispatch unchanged. New
cross-cutting behavior enters as systems; existing per-ship behavior stays in
slots — but after review-005 a slot value must be a thin system function
over the entity's components: behavior state lives in components (Guided,
Spin, Cloak), optional phases are component presence (WarpingIn,
Exploding — the C's per-instance hook mutation has no successor), and the
ship phases themselves are named systems called in pinned order.

Traversal order is *data*, all the way down (review-006): every spawn
names its stratum, and `Order{layer, seq}` on the entity is the whole
story — there is no list. `Battle::eachOrdered` sorts and walks where
order is gameplay (RNG-drawing passes, targeting tie-breaks, pair order,
draw); plain views iterate everywhere else. The frame itself is a declared
pipeline of batch systems with one sync point; cross-entity reads see the
frame-start snapshot, structural effects travel as commands, and ship
damage stacks in an effect component applied once per hull. What the C
encoded by head/tail insertion tricks (pkunk.c:498-512's head-inserted
phoenix) is a layer declaration here; what it encoded by hook
self-mutation is component presence.

## Migration plan

- **Now**: specs. `ShipData`'s prefixed field blocks become nested
  `WeaponSpec`/`SpecialSpec` values; ship definitions become spec literals —
  declarative in code before any file format exists.
- **At M2, per mechanic**: registry components. `ShipState` is one
  already (review-004 X3); each subsequent mechanic (limpets, tethers,
  charge state) is a new component type, never a new `Element` field —
  and review-004's split rule applies: a component needs an owner
  narrower than "everything", never taxonomy. The plan's rule holds: no
  component is generalised until its second user exists.
- **Never, absent a profile**: archetype storage, SoA, parallel systems.

## The promotion rule

**If a behavior needs an in-place patch — a kind-check, a special case inside
a shared function — that is a signal the concern belongs one scope up.**
Worked example: the `Laser` checks that were sprinkled through the step loop
(skip the Appearing seed, skip the commit) were one concern — "this
element's two points are geometry, not motion" — wearing disguises; they are
now the `BeamGeometry` trait flag, set at spawn, and the step loop knows no
element kinds at all. When a patch like this appears, promote it or file
it; do not add another copy.

## Faithfulness policy

- The game should FEEL the same; frame-exactness was given up deliberately
  (game-rewrite-plan.md, §What we accept losing).
- Behavior is ported from the C source with citations, not from memory —
  review-001 documents what happens otherwise.
- It is acceptable to sidestep genuinely tricky mechanisms nobody can defend
  (display-grid impact snapping, PD's collidable laser elements) — but every
  sidestep is recorded in `src/docs/design-notes.md`'s divergence ledger, so
  a later difference is attributable.
