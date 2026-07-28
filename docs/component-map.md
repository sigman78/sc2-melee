# The component map

Every type the registry stores, grouped by the question it answers. The
authority is the code; this is the index you read first.

Written after review-007 closed, when the model stopped moving: `Element`
is gone, so *composition is the whole description of a thing*. There is no
struct left to read to find out what an entity is — you ask which
components it carries.

## How to read this

- A type marked `comp` (the empty marker macro in each header) is a
  registry-attached component. An empty one is a **tag**: presence is the
  whole value.
- **Minimal composition** — a component is attached only where some pass
  reads it. An entity missing one is not incomplete; it is a thing that
  pass does not concern. `Allegiance` is the single deliberate exception,
  attached by every spawn.
- **The join rule** — a pass declares its read-set in its
  `each<Ts...>`/`eachOrdered<Ts...>` call, filters presence with
  `entt::exclude`, and reaches for `find<T>` only for a conditional read of
  *another* entity.
- Four values belong to the world rather than to any entity and live in the
  registry's context, reached through `Battle::setContext/context/findContext`.

## Placement and traversal

| Component | Answers | Carried by |
| --- | --- | --- |
| `Position{current, next, facing}` | where it is, where the step is taking it, which way it points | everything that occupies a point |
| `Beam{from, to}` | the two *ends* of a drawn line | beams only — a beam has no `Position`, so Integrate and Commit never see one |
| `Motion{velocity}` | how fast, in packed fixed point | anything that moves under its own or inherited velocity |
| `Order{layer, seq}` | its declared position in the frame's walk | every sim spawn, via `make()` |

`Order` is also what separates a stepped element from an app-owned entity:
`buildOrderedIds` keys on it, so anything built with `Battle::create()`
stays out of the walk entirely.

## Substance — what can touch what

| Component | Answers | Carried by |
| --- | --- | --- |
| `Physique{mass}` | how hard it is to push, and whether it pulls | everything collidable |
| `Collider{mask}` | **is it solid** — solidity is having one | anything that can be hit right now |
| `StashedMask{mask}` | a mask held while *not* solid | see the overlap note below |
| `CollisionScratch{collided, defyPhysics}` | this frame's contact bookkeeping | anything the collide pass walks |
| `PriorSilhouette{mask, facing}` | the silhouette it entered the frame with | private to `Battle.cpp` — the overlap-repair protocol's own scratch |

## Life and death

| Component | Answers | Carried by |
| --- | --- | --- |
| `Appearing` | born this frame, exempt from its own collisions | every `spawn()`, cleared at the first frame's end |
| `Lifetime{remaining}` | a countdown to zero | transients; **absent means persistent** |
| `Doomed` | marked for this frame's reap; its death response already ran | anything dying |
| `Indestructible` | immune to weapon damage, nothing to age | the planet |
| `DeathSpawn{emit}` | what to leave behind when it dies | asteroid, rubble |
| `SweepsOwnedOnDeath` | destroy everything I own when I die | a dying ship |

`DeathSpawn` and `SweepsOwnedOnDeath` are mutually exclusive per entity —
together they are what the C's `onDeath` hook became.

## Allegiance and identity

| Component | Answers | Carried by |
| --- | --- | --- |
| `Allegiance{playerNr, owner}` | whose it is, and what fired it | **everything** — the one uniform attach |
| `PlayerShip` | is this a player's ship | ships (see overlap note) |
| `IgnoreSimilar` | skip collisions with things sharing my owner | ships, the flame |

## Damage

| Component | Answers | Carried by |
| --- | --- | --- |
| `Vitality{hitPoints}` | how much punishment left | planet, asteroids, shots — **never a crewed hull** |
| `Warhead{damage, blastOffset, lingersOnHit}` | what it does on contact | weapon shots |
| `DamageIncoming{amount, lastFrom}` | damage summed this frame, applied at the sync point | ships being hit; cleared every frame |

A ship's toughness is its crew (`ShipState`), never `Vitality`. Every
damage site branches on ship-vs-not before it reads either.

## The ship

| Component | Answers | Carried by |
| --- | --- | --- |
| `ShipState{spec, crew, energy, counters, speed, inGravityWell, turnWait, thrustWait}` | everything mutable about a ship | ships |
| `Input{buttons}` | this frame's intent | ships |
| `Cloak{level}` | how far into the cloak ramp | the Ilwrath, lazily on first use |
| `Cloaked` | fully cloaked — invisible to eye and targeting | maintained solely by the cloak machine |
| `WarpingIn` | materialising, not yet solid | ships during arrival |
| `Exploding` | burning down | a dying ship |

Optional phases are component presence; there is no phase enum and no
per-instance hook.

## Ordnance

| Component | Answers | Carried by |
| --- | --- | --- |
| `WeaponGuidance{spec}` | the `WeaponSpec` this shot came from | every weapon shot (see naming note) |
| `Guided{trackWait, maxSpeed, thrustScale, clock}` | it steers, and here is its tracking clock | guided shots only — the Cruiser's nuke |
| `AnimFrame{n}` | which cel to draw | shots |
| `FrameDriven` | advance `AnimFrame` every frame I live | the Ilwrath flame alone |

## Render taxonomy — sim-owned tags the passes key on

| Tag | Drawn by |
| --- | --- |
| `Planet` | `renderPlanet` — also how `gravityPass` finds the one well |
| `Spin{backwards, period, countdown}` | `renderAsteroids` — not a tag; it also *is* the tumble |
| `Trail` | `renderEffects`, stepping the ion ramp by age |
| `Shadow` | `renderEffects`, the warp-in silhouette |
| `Debris` | `renderEffects`, a spark of a dying ship |
| `Blast` | `renderEffects`, an impact flash |

These are the only sim components that exist for the renderer's benefit,
and `Planet`/`Spin` both earn their place sim-side anyway.

## App-side

| Component | Answers |
| --- | --- |
| `Visual{sprites, fallback}` | which art, and what to draw if it is missing |
| `Starfield{stars}` | the whole parallax field — **one entity**, no `Position`, no `Order` |
| `Mark{event, bornFrame}` | one recorded collision, reaped by age |
| `AnnouncedDead` | this ship's death sound already played |

## Context singletons

| Type | Holds |
| --- | --- |
| `MatchState{winner, endedAtFrame, shipIds}` | how the match stands |
| `game::Camera` | what the passes render through |
| `DebugToggles{overlay, wasDown}` | F1 state |
| `BattleConfig{roster, shipData}` | the two ships' definitions and spec storage |

## Worked examples — what things are made of

Read these as the answer to "what *is* a shot", now that no struct says so.

**A player's ship**
`Order Position Motion Physique Allegiance CollisionScratch PriorSilhouette
Appearing Collider IgnoreSimilar PlayerShip ShipState Input` + `Visual`
— plus `WarpingIn Lifetime StashedMask` while arriving (its `Collider` is
detached for the duration), `Cloak`/`Cloaked` if it is the Ilwrath, and
`Exploding SweepsOwnedOnDeath` when it dies.

**An asteroid**
`Order Position Motion Physique Allegiance CollisionScratch PriorSilhouette
Appearing Collider Spin Vitality{1} DeathSpawn StashedMask` + `Visual`.

**The planet**
`Order Position Motion Physique Allegiance CollisionScratch PriorSilhouette
Appearing Collider Indestructible Planet Vitality{200}` + `Visual`.
Its mass is assigned from its hit points *after* placement, so the
placement loop can ask "is this spot inside someone's well" without the
planet answering about itself.

**A weapon shot**
`Order Position Motion Physique Allegiance CollisionScratch PriorSilhouette
Appearing Collider Warhead Vitality Lifetime AnimFrame WeaponGuidance` +
`Visual` — plus `Guided` if it steers, `FrameDriven` if it grows,
`IgnoreSimilar` if it must not burn its own hull.

**A beam** (the point-defence laser)
`Order Beam Allegiance Lifetime{1}` + `Visual`. Nothing else — no
`Position`, no `Motion`, no collision scaffold at all. The collide pass
gates on `collidable()` before it would ever read through the hole.

**An ion trail point**
`Order Position Allegiance Lifetime Trail` + `Visual`. Its position is set
once and never touched again, so it carries no `Motion` — unlike a debris
spark, which is the one decoration that drifts and so adds it.

**A collision mark** (app-owned)
`Mark` alone, on a bare `Battle::create()` entity — no `Order`, outside
`size()`, invisible to every sim pass.

## Overlaps found

A pass over the finished model for duplication. Four things are worth
knowing; two are worth changing.

**1. `PlayerShip` and `ShipState` are the same predicate.** In production
they are attached together (`spawnPlayerShip` → `attachShip`) and never
apart, and the code already relies on it — `ShipSystems.cpp` and
`Targeting.cpp` both carry comments saying `PlayerShip` need not be checked
separately because only a ship carries a `ShipState`. So the model states
"is a ship" twice. They are read for different reasons — `PlayerShip` by
collision, gravity, damage routing and field placement, `ShipState` by the
ship systems — but that is a difference in *caller*, not in meaning. Either
`PlayerShip` goes and those sites ask `has<ShipState>`, or it stays as the
cheap tag for hot paths and the equivalence gets stated once as an
invariant instead of twice as an aside. Worth a decision; it is the
clearest redundancy left.

**2. `StashedMask` means two different things.** On an asteroid it is a
durable copy of the birth mask that deliberately coexists with the
`Collider` so the death chain can hand the mask to the next asteroid. On a
warping-in ship it is a temporary parking spot, attached as the `Collider`
is detached and removed on arrival — strictly exclusive with it. Same
component, opposite lifetimes and opposite relationships to `Collider`, so
"does this entity have a StashedMask" has no single meaning. Splitting it
(`BirthMask` for the asteroid's, `ParkedMask` for the ship's) would make
each site's invariant checkable.

**3. `WeaponGuidance` does not guide anything.** It holds the shot's
`WeaponSpec` borrow, and is read for collision masks and cel lookup as much
as for steering; the component that actually steers is `Guided`. Two
near-identical names for unrelated jobs, which is a reading tax every time.
A rename (`FromWeapon`, or fold it behind the existing `weaponSpec()`
accessor) costs nothing semantically.

**4. Three clocks drive animation, and that is correct.** `AnimFrame{n}`
for shots, `Spin{countdown, period}` for the asteroid tumble, and
`Lifetime{remaining}` read backwards as age for trails, shadows and debris.
They look redundant and are not: each is the value its own system already
had to maintain, and unifying them would mean giving a trail a counter that
duplicates its lifetime. Recorded so the next reader does not re-litigate it.

**Not duplication, though the shapes match:** `Position{current, next}` and
`Beam{from, to}` are both two points, meaning motion and geometry
respectively — the split exists precisely because the old code abused one
for the other. `Collider{mask}` and `StashedMask{mask}` share a shape for
the reason in (2).

**One inconsistency:** `MatchState`, `DebugToggles` and `BattleConfig` are
marked `comp`, but they are context singletons and never attach to an
entity. The marker's own definition says it means "an
`entt::registry`-attached type", so it is claiming something untrue of all
three. `game::Camera` sits beside them in the context unmarked — for the
different reason that it is a class with behaviour, reused as-is rather
than written as context state. Either the three lose the marker or the
context gets a marker of its own.
