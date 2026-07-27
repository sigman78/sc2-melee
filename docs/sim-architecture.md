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

**Storage** — archetype/SoA arrays, entities as indices into them, systems as
tight loops over contiguous components. Adopted **not at all**, for two
falsifiable reasons:

1. There is no performance problem to solve. A melee is ~40 entities at
   24 Hz; the WASM budget is spent in rendering and decoding, not the step.
2. **Traversal order is gameplay.** The Pkunk phoenix is head-inserted so it
   preprocesses before the dead ship's death hook; AI target selection and
   the collision walk's pair order follow list order. Archetype storage
   reorders entities by composition, which fights the one invariant the C
   actually depends on.

If a profile ever contradicts reason 1, this file is where the argument gets
reopened — not a refactor that quietly assumes it.

## Vocabulary

| Term | What it is here | What it is in the C |
| --- | --- | --- |
| **Entity** | `EntityId` — stable, generation-checked handle in an explicitly ORDERED `EntityList` | `HELEMENT` in `disp_q` |
| **Component** | Typed state blob keyed by `EntityId`, stored in per-kind sidecars; `Element` keeps only the universal motion/collision core | fields smuggled into `ELEMENT` or `STARSHIP` |
| **Behavior slot** | The four phase hooks — pre, post, collision, death — filled from the component library, per entity | per-instance function pointers |
| **System** | A cross-entity pass with its own state: gravity, camera, sound | special-cased calls inside the queue walk |
| **Event** | Observational output of `step()` — `CollisionEvent`, `SpawnEvent`. Never an input; nothing reads events back into the sim | draw/sound calls made mid-step |
| **Spec** | Immutable declarative description — `ShipSpec{thrust, weapon, special, ai}` built from `WeaponSpec`/`SpecialSpec` values. In code today, the TOML of the plan tomorrow; same shape either way | `RACE_DESC` plus per-ship `#define`s |

Hooks are deliberately NOT an event bus. Every ship in `ships/` was tuned
against direct per-entity dispatch, and M2's strategy (port `cyborg.c`
against a stable reference frame) needs that dispatch unchanged. New
cross-cutting behavior enters as systems; existing per-ship behavior stays in
slots.

## Migration plan

- **Now**: specs. `ShipData`'s prefixed field blocks become nested
  `WeaponSpec`/`SpecialSpec` values; ship definitions become spec literals —
  declarative in code before any file format exists.
- **At M2, per mechanic**: sidecar components. `ShipState` moves out of
  `Element` (every asteroid currently carries one) when the first new ship
  forces the store's shape; each subsequent mechanic (limpets, tethers,
  charge state) is a new component, never a new `Element` field. The plan's
  rule holds: no component is generalised until its second user exists.
- **Never, absent a profile**: archetype storage, SoA, parallel systems.

## The promotion rule

**If a behavior needs an in-place patch — a kind-check, a special case inside
a shared function — that is a signal the concern belongs one scope up.**
Standing example: the `Laser` checks sprinkled through the step loop (skip
the Appearing seed, skip the commit) are one concern — "this element's two
points are geometry, not motion" — wearing three disguises; it wants to be a
per-kind trait. When a patch like this appears, promote it or file it; do
not add a fourth copy.

## Faithfulness policy

- The game should FEEL the same; frame-exactness was given up deliberately
  (game-rewrite-plan.md, §What we accept losing).
- Behavior is ported from the C source with citations, not from memory —
  review-001 documents what happens otherwise.
- It is acceptable to sidestep genuinely tricky mechanisms nobody can defend
  (display-grid impact snapping, PD's collidable laser elements) — but every
  sidestep is recorded in `src/docs/design-notes.md`'s divergence ledger, so
  a later difference is attributable.
