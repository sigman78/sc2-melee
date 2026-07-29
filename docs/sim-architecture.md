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

**Storage** — amended on the rewrite/ecs branch by reviews 004 and 006.
Adopted **as EnTT sparse-set pools, and only that**: an `entt::registry`
stores components keyed by entity; traversal order is the `Order{layer,
seq}` component, sorted on demand by `Battle::eachOrdered` where order is
gameplay and ignored by plain views where it is not (the OrderLink spine
of review-004 was the transitional form; review-006 retired it). The two
falsifiable reasons the original decision rested on, re-examined against
the executed code:

1. There is no performance problem to solve — still true, and nothing in
   the adoption was done for speed. Pools were adopted for the composition
   ergonomics (a component is a type and an `emplace`, not a hand-rolled
   sidecar), not for cache behaviour.
2. **Traversal order is gameplay** — held as data, and since measured, so
   it can now be stated more precisely than it was assumed. The order never
   entered the library's storage: first an owned spine, finally a sort key.
   But reversing `seq` within a layer across all 32 replay battles changed
   **no** observable outcome — same winner every battle, same crew lost,
   same shots, same collisions, same end frames. Roughly twelve contacts per
   two-thousand-frame battle leaves within-frame ordering almost nothing to
   sequence, and the collision response normalises the ship case anyway.

   So the claim splits. **Layer order is gameplay** — the strata are
   declared, and a mechanic that must act before another (the Pkunk phoenix
   preprocessing ahead of a dying ship) says so by naming a layer.
   **Within-layer order is determinism, not gameplay** — `seq` earns its
   place as the tiebreak that keeps the sort total and reproducible, since
   entt's pool order is unstable under deletion and the replay digest folds
   state in walk order. Changing the walk order would cost one baseline
   re-record and nothing else.

   What this reason rejects is unchanged: archetype/SoA storage that
   reorders entities by composition stays rejected.

The costs paid for the pools are recorded in review-004's friction ledger
(3-4× per-TU compile time in sim/, the EntityRef diagnostic lost, the
kNoEntity/in_place_delete/spawn-then-tag disciplines). If a profile ever
contradicts reason 1 in the other direction, this file is still where the
argument gets reopened — not a refactor that quietly assumes it.

## Vocabulary

| Term | What it is here | What it is in the C |
| --- | --- | --- |
| **Entity** | `EntityId` = `entt::entity` — a versioned id and nothing else; what it *is* is which components it carries | `HELEMENT` in `disp_q` |
| **Component** | A type in the registry, `emplace`d per entity (`Position`, `ShipState`, `Cloak`, tags, the app's `Visual`). There is no universal core struct | fields smuggled into `ELEMENT` or `STARSHIP` |
| **Behavior slot** | A ship's `preProcess` and a special's activation `hook`, both spec-level | per-instance function pointers |
| **System** | A cross-entity pass with its own state: gravity, camera, sound | special-cased calls inside the queue walk |
| **Event** | Observational output of `step()` — `CollisionEvent`, `SpawnEvent`. Never an input; nothing reads events back into the sim | draw/sound calls made mid-step |
| **Spec** | Immutable declarative description — `ShipSpec{thrust, weapon, special, ai}` built from `WeaponSpec`/`SpecialSpec` values. In code today, the TOML of the plan tomorrow; same shape either way | `RACE_DESC` plus per-ship `#define`s |

**There are no per-entity hooks.** Behaviour is data plus dispatch on
composition: `onCollision` became a branch on `has<Warhead>` —
`weaponCollision` for a shot, `solidCollision` for anything else — taking
the other id as an argument, so nothing stores who it collided with.
`onDeath` became a `DeathSpawn{emit}` payload and the `SweepsOwnedOnDeath`
tag. Behaviour state lives in components (`Guided`, `Spin`, `Cloak`), and
optional phases are component presence (`WarpingIn`, `Exploding`). Nothing
swaps a function pointer on a live entity the way `chmmr.c:773` does.

What stays per-ship is spec-level: a ship's `preProcess` (the Ilwrath cloak
machine) and a special's activation `hook`. These are deliberately NOT an
event bus — every ship in `ships/` was tuned against direct dispatch, and
M2's strategy (porting `cyborg.c` against a stable reference frame) needs
that dispatch unchanged. New cross-cutting behaviour enters as a system.

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

## Where this stands

- **Done**: specs are nested values and ship definitions are declarative
  literals; every mechanic is a registry component; there is no `Element`
  left to add a field to. The full sequence is in `src/docs/review-002`
  through `review-009`.
- **Per mechanic, from here**: a new mechanic (limpets, tethers, charge
  state) is a new component type. The split rule applies — a component
  needs an owner narrower than "everything", never a taxonomy — and nothing
  is generalised until its second user exists.
- **Never, absent a profile**: archetype storage, SoA, parallel systems.

## The promotion rule

**If a behavior needs an in-place patch — a kind-check, a special case inside
a shared function — that is a signal the concern belongs one scope up.**
Worked example, carried to its end: the `Laser` checks sprinkled through the
step loop (skip the Appearing seed, skip the commit) were one concern — "this
element's two points are geometry, not motion" — wearing disguises. Promoting
them to a trait flag removed the checks; promoting them again removed the
flag. A beam now carries `Beam{from, to}` and no `Position` at all, so the
passes that move things never match it and there is nothing to exempt. The
same shape twice: `gravityPass` stopped scanning every mass for the one
gravity well when the well started carrying a `Planet` tag.

When a patch like this appears, promote it or file it; do not add another
copy. The strongest version of the promotion is usually the one where the
special case becomes structurally unrepresentable rather than merely
flagged.

## Faithfulness policy

- The game should FEEL the same; frame-exactness was given up deliberately
  (game-rewrite-plan.md, §What we accept losing).
- Behavior is ported from the C source with citations, not from memory —
  review-001 documents what happens otherwise.
- It is acceptable to sidestep genuinely tricky mechanisms nobody can defend
  (display-grid impact snapping, PD's collidable laser elements) — but every
  sidestep is recorded in `src/docs/design-notes.md`'s divergence ledger, so
  a later difference is attributable.
