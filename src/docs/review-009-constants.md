# Review 009 — constants that know what they belong to

SiGMan's ask, from a read of the finished ECS: scalar pairs that are
obviously one compound value (`kScreenWidth`/`kScreenHeight` is an
`Extent2i`), field clusters that beg to be a nested struct
(`maxEnergy`/`energyRegen`/`energyWait` is a battery), and free constants
that belong on the type whose behaviour they govern (`kWarpInFrames` is
`WarpingIn`'s business, not the namespace's).

Nothing here changes behaviour. The replay baseline is the gate throughout,
and the only stage that could move a value is the arithmetic folding in §5.

## 1. The rule

The sweep needed a tiebreaker, and `CollisionEvent` supplied it. Its
`beforeA`/`beforeB`/`afterA`/`afterB` can group two ways — by time
(`before{a,b}`) or by participant (`a{before,after}`) — and the deciding
question is not which reads nicer:

> **Group by the axis the operations are local to, not the axis the writes
> happen on.**

`after - before` is the velocity change, and it means something only within
one participant; nothing anywhere reads "both befores" as a unit. The
producer does fill the fields by time — `beforeA`/`beforeB` before the
response runs, `afterA`/`afterB` after — but that is an artifact of when
values become available, not of what they mean. A type shaped around its
fill order is a type nobody can read.

The rule earns its keep by disqualifying suggestions as well as confirming
them. `Stagger{turn, thrust}` and `Cooldowns{weapon, special}` were both
proposed and both dropped: those fields are decremented and read
independently, so no operation is local to the pair. `Battery` survives
because regen is one operation reading all three.

## 2. The defect the sweep found

`Draw.hpp` declares `kStarCount = 30 + 60 + 90`. `Draw.cpp` declares
`kStarsPerPlane{{30, 60, 90}}`. The same three numbers, written twice, in
two files.

`Starfield::stars` is sized by the first; `renderStars` walks the second,
accumulating `first += count` and indexing into that array. Raising a
plane's star count edits one and silently overruns the other. This is the
only item in the review that fixes a bug rather than a reading experience,
so it goes first and alone.

## 3. Constants move to their types

A constant that describes how one component behaves belongs to that
component:

| Was | Becomes |
| --- | --- |
| `kWarpInFrames`, `kTransitionSpeed` | `WarpingIn::kFrames`, `WarpingIn::kImageSpacing` |
| `kExplosionFrames`, `kExplosionLife`, `kHullVanishAge` | `Exploding::kFrames`, `::kLife`, `::kHullVanishAge` |
| `kDebrisLife` | `Debris::kLife` |
| `kIonTrailLife` | `Trail::kLife` |
| `kBlastLife` | `Blast::kLife` |
| `kCloakVisibleColours`, `kCloakFullLevel` | `Cloak::kVisibleColours`, `Cloak::kFullLevel` |
| `kMarkLife` | `Mark::kLife` |

Call sites gain the subject they were missing: `kIonTrailLife -
lifeSpanOf(b, id)` becomes `Trail::kLife - lifeSpanOf(b, id)`, which says
whose life it is.

**The tags stay tags.** A `static constexpr` member does not make a class
non-empty — "empty" means no *non-static* data members — so
`std::is_empty_v` still holds, entt's empty-type elision is unaffected, and
the ordered walk keeps working. That is load-bearing enough to pin with a
`static_assert` rather than trust.

One constant cannot travel: `kTransitionSpeed` is `displayToWorld(40)` and
needs `World.hpp`, which `Entity.hpp` does not include. It lands on
`WarpingIn` (in `Ship.hpp`, which does) rather than on `Shadow`.

## 4. Clusters become types

**`CollisionEvent`** groups by participant, absorbing the ids that were
already grouped that way:

    struct CollisionEvent
    {
        struct Side
        {
            EntityId id = kNoEntity;
            Vec2i before;   // world units per frame
            Vec2i after;
        };
        Side a;
        Side b;
        Vec2i at;
    };

`a`, `beforeA` and `afterA` were three fields describing one entity,
scattered.

This section first argued for a `delta()` accessor on `Side`, on the
grounds that `renderMarks` wants the difference — its comment says "the
difference *is* the response". Reading the pass settled it the other way:
it draws four arrows out of the contact point, one along each raw vector,
so the difference is what the *viewer* sees between two drawn arrows, not
a value the code computes. `delta()` would draw a fifth, different line.
No consumer wants it, so it is not written. The grouping stands on its own
argument: `after - before` is meaningful only within one participant,
whether or not anything spells it today.

**`ShipSpec`** gets `Battery{max, regen, wait} battery` in place of
`maxEnergy`/`energyRegen`/`energyWait`, following `ThrustProfile thrust`,
which already sits beside it as the precedent.

## 5. Sizes become sizes

`Extent2` today carries `==`, `empty()`, `area()` and `contains()` — no
arithmetic — so folding a width/height pair into one buys a name but leaves
every use site reading `.w` and `.h`. To get the arithmetic, `Extent2`
gains component-wise operators.

**Decided: extend `Extent2` rather than represent sizes as `Vec2i`.** The
free operators are tempting, but `Extent2` exists precisely so a size
cannot be passed where a point is wanted, and that distinction is worth
more than the code it saves.

| Was | Becomes |
| --- | --- |
| `kLogSpaceWidth`, `kLogSpaceHeight` | `kArena : Extent2i` (63 uses; `wrapX`/`wrapY` collapse into `wrap`) |
| `kSpaceWidth`, `kSpaceHeight` | `kSpace : Extent2i` |
| `kScreenWidth`, `kScreenHeight` | `kScreen : Extent2i` |
| `kSafeX`, `kSafeY` | `kSafe : Vec2i`, or deleted — both are zero and feed only the space computation |
| `kStarFieldWidth`, `kStarFieldHeight` | `kStarField : Extent2i` |
| `kTransitionWidth`, `kTransitionHeight` | `kTransition : Extent2i` |
| `kHysteresisX`, `kHysteresisY` | `kHysteresis : Vec2i` — an offset, so a point type, not a size |

This is the one stage where a fold could change an integer expression's
result. It runs last, and alone.

## 6. The stages

| Stage | What | Proof |
| --- | --- | --- |
| K1 | `kStarCount` derives from `kStarsPerPlane`; the three parallel per-plane arrays become one array of `StarPlane{count, colour, cel}` | bit-green; the fix |
| K2 | Constants move onto their components (§3), with the emptiness `static_assert` | bit-green |
| K3 | `CollisionEvent::Side` and `ShipSpec::battery` (§4) | bit-green |
| K4 | `Extent2` gains operators; the size pairs fold (§5) | bit-green; the only stage that touches arithmetic |
