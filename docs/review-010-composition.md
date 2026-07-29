# Review 010 — components get a namespace, entt gets its surface back

SiGMan's ask, six threads at once: components and tags share a namespace
with ordinary classes and cannot be told apart; `DeathSpawn` carries a
callback; `StashedMask`'s use is a stretch; `Lifetime`/`Doomed`/`lifeSpanOf`
still read wrong; weapons and specials need a shape that accommodates every
SC2 ship without multi-point patching; and `Battle`'s entt wrappers add
friction without value.

The bar is review-007's and review-008's: **bit-green, the replay baseline
never moves.** Every stage below is bit-exact by construction or by a stated
argument, with one deliberate exception that is a throwaway spike rather
than a stage.

## 1. Three findings that reshaped the ask

**`Damage.cpp:120` is a tautology.**

```cpp
&& (isFiniteLife(b, targetId) || lifeSpanOf(b, targetId) == 1)
```

`lifeSpanOf` returns `1` exactly when there is no `Lifetime`, so the second
clause is implied by the negation of the first: the disjunction is
unconditionally true. It is the residue of the C's `lifeSpan = NORMAL_LIFE+1`
planet encoding (`misc.c:55`, cited at `Field.cpp:141`) — which the
`Indestructible` tag replaced, two lines up in the same condition. The same
shape makes the outer guard at `Battle.cpp:351` redundant: the inner test can
only pass for a side that holds a `Lifetime`, which is what the guard asks.

So the `lifeSpanOf` complaint has a body. The magic `1` is a C encoding that
outlived its readers, and it is hiding dead code in two of its seven call
sites.

**`StashedMask` and `DeathSpawn` are one mechanic wearing two generic
names.** After review-007 removed the warp-in use, `StashedMask` has exactly
one owner: the asteroid → rubble → asteroid recycle. `DeathSpawn`'s function
pointer has exactly two values and both only call `queueSpawn` — one hands
the mask on, the other spends it. Review-009's rule disqualifies a grouping
that names a thing which does not exist; the inverse applies here, where a
function pointer names a generality that does not exist.

**`SpawnCommand`, not `SpecialSpec`, is the obstacle to accommodating every
ship.** It is a 25-field struct with one `optional<>` per component, four
bools that select an archetype, and a `deferred` escape hatch. Every new
component on a spawned entity costs a field *and* a branch in
`drainSpawnCommands`. `SpecialSpec::pointDefenceRange` is the same defect at
one tenth the size.

## 2. The namespace

The `comp` marker macro and a namespace named `comp` cannot coexist, and the
namespace makes the macro redundant: `comp::Position` states it at every
*use* site instead of only at the definition. Six `#define comp`/`#undef comp`
pairs delete.

Groups are `inline namespace`s, so `comp::life::Doomed` and `comp::Doomed`
both compile and the everyday spelling stays the short one. The reason is
evidence, not taste: the categories moved twice while this review was being
written — `Planet` and `Spin` each changed group, and `Blast` is a render
decoration to the component map and a weapon effect to the person who asked
for the sweep. Both readings are right. **Categories that move are a bad
thing to pin into 1300 call sites**, and an inline namespace makes
re-grouping one header edit instead of a tree sweep.

| Group | Holds |
| --- | --- |
| `comp::space` | `Position`, `Beam`, `Motion`, `Order`, `Spin` |
| `comp::matter` | `Physique`, `Collider`, `CollisionScratch`, `Planet`, `StashedMask` |
| `comp::life` | `Appearing`, `Lifetime`, `Doomed`, `Indestructible`, `SweepsOwnedOnDeath`, `DeathSpawn` |
| `comp::owner` | `Allegiance`, `IgnoreSimilar` |
| `comp::harm` | `Vitality`, `Warhead`, `DamageIncoming` |
| `comp::ship` | `ShipState`, `Input`, `WarpingIn`, `Exploding`, `Cloak`, `Cloaked` |
| `comp::shot` | `FromWeapon`, `Guided`, `AnimFrame`, `FrameDriven` |
| `comp::look` | `Trail`, `Shadow`, `Debris`, `Blast` |

Lowercase, matching `uqm::sim` and `uqm::game`. App-side components are
`melee::comp` — flat, four types, no group worth naming. The context
singletons review-008 §5 unmarked are `melee::ctx::{MatchState,
DebugToggles, BattleConfig}`; `game::Camera` keeps its own namespace, being
a class reused as context rather than context state.

Three of the originally proposed groups did not survive. `Tags::` is not a
category — tag-ness is emptiness, and it would swallow more than half the
model. `Sfx::` has one member and it is app-side. `Cleanup::` and
`Lifecycle::` split a set no reader splits.

## 3. The entt surface

`view` ×4, `context` ×4, `ordered`, `attach`, `attachOrReplace`, `get` and
`detach` are pure pass-throughs. `find` and `has` add a `valid(id)` check —
real, since `try_get` on a stale handle is undefined — but most call sites
already open with `if (!b.alive(id)) return;`. `eachOrdered` is the only
wrapper with a mechanism behind it, and `ordered<Ts...>()` already exposes
that mechanism as a view.

So: `entt::registry reg` becomes a public member, every pass-through is
deleted, and what stays is what has semantics — `ordered`, the `spawn`
family, `alive`, `collidable`, `queueSpawn`, `step`, `rng`.

Two changes make that safe rather than merely permitted:

- `count_` is deleted; `size()` reads `reg.storage<Order>().size()`, which
  is what the counter already tracked. An outside `destroy` can no longer
  desynchronise it.
- `orderDirty_` stops being set by hand. `on_construct<Order>` and
  `on_destroy<Order>` observers set it, so sortedness holds no matter who
  touches the pool.

**This reverses review-004 §3 and review-005 §5**, which settled on
"`entt::registry` never escapes `Battle`" and made the app work through the
typed surface. Review-008 V1 already reversed half of it for views. The
reversal is deliberate: the query vocabulary is entt's, this codebase will
need more of it than a wrapper set can anticipate, and the two invariants
the wrapper was protecting are now derived rather than maintained.

## 4. Stages

| Stage | What | Proof |
| --- | --- | --- |
| W1 | `comp::` with inline groups; the `comp` macro deleted; `melee::comp` and `melee::ctx` | **done, bit-green** — 28 files, all 32 battles matched exactly |
| W2 | The entt surface goes: public `reg`, pass-throughs deleted, `count_` and `orderDirty_` derived | pending |
| W3 | `lifeSpanOf` says what it means: both dead clauses deleted, `framesLeft` asserts, `isFiniteLife` becomes `isTransient` | pending |
| W4 | `comp::Asteroid{mask, phase}` replaces `DeathSpawn` and `StashedMask` | pending |
| W5 | Spawns are built, not described: eager creation with `Order` withheld; `SpawnCommand` deleted | pending |
| W6 | Specials: `SpecialSpec` is the gate only; `PointDefence` and `Cloak` are components; `preProcess` and `hook` deleted | pending |

W1 and W2 sweep the same files and land together, as two commits.

### W3 — what stays

`Lifetime` and `Doomed` keep their names; they are not the defect, and the
component map's state table still holds. What changes is the function that
lied about them. `lifeSpanOf` served three different questions — remaining,
age, and is-persistent — and encoded the third as the magic `1`. After the
two dead clauses go, its four survivors all run on entities known to hold a
`Lifetime`, so it becomes `framesLeft`, which asserts. Debug builds are the
suite's assertion coverage.

One detail is load-bearing: `replay_test.cpp:252` folds `lifeSpanOf` into
the digest for **every** entity, including those with no `Lifetime`, where
it returns 1. The test keeps that exact expression spelled out, or the
baseline moves for a reason unrelated to the change.

### W4 — the name

`Asteroid{mask, phase}` with `Phase::Solid | Phase::Rubble`. It names what
the entity is rather than what the engine does to it, which is the register
the rest of the model uses (`Doomed`, `Warhead`, `Spin`, `Planet`). It also
makes the two-step legible: the rubble is a five-frame timer carrying a
mask, not a second mechanic.

Cost: three `sim_test` cases attach `DeathSpawn` as a *generic* death
payload (`sim_test.cpp:834`, `:877`, `:2014`). That property stops existing,
which is the point, and those tests are rewritten against the concrete
mechanic.

### W5 — why eager creation works

`SpawnCommand` exists for **visibility**, not creation: a mid-frame spawn
must not be seen, collided with or steered toward that frame
(`Battle.hpp:71`). entt lets those be separated, because `Order` is already
the membership token — the component map states that anything without one
"stays out of the walk entirely", and `count_` is the `Order` pool size.

So the entity is created when the pass asks, every component is attached
through the ordinary `.with()` chain, and `Order` is withheld. The sync
point does one thing: `emplace<Order>` in emission order, and `recordSpawn`
there, which keeps today's "flavor derived after all components attach"
invariant for free.

Six passes currently iterate un-ordered views a pending entity would appear
in — `capturePriorPass`, `integratePass`, `ageDecrementPass`, `commitPass`,
`flagsEndOfFramePass`, and `animatePass`'s `<AnimFrame, FrameDriven>`
sub-pass. Each joins `Order`. That is not scaffolding for the trick; it is
"only stepped elements step" becoming true of every pass instead of only the
ordered ones. `flagsEndOfFramePass`'s `clear<Appearing>()` becomes a walk
over `<Order, Appearing>` — which is *why* a newborn keeps `Appearing` into
its first live frame. Today that depends on pass ordering; afterwards it is
a stated join.

One escape hatch survives, and only one: `rubbleDeath` must draw its RNG at
the sync point in queue order, so it stays a thunk. A second one appearing
is the signal to revisit.

The failure mode is loud: miss a pass and a newborn steps a frame early,
which the baseline flags on the first battle.

**Prototypes and clone were considered and deferred.** entt has no
first-class clone; it is a type-erased loop over `reg.storage()` doing
`push(dst, storage.value(src))` per pool, touching all ~40 storages per shot
and copying components the caller is about to overwrite. It earns its keep
when specs come from *data files*, because then "a nuke is this bundle" has
to be expressible without C++. Ship specs are designated initialisers in
`ships/*.cpp` today, so the indirection buys nothing the `.with()` chain
does not. Revisit if ship data moves into content.

### W6 — and what was rejected

`SpecialSpec` keeps the part that is genuinely uniform — every ship's
special is a cost plus a debounce, and the engine only ticks the counter
(`ship.c:342-346`). Everything else leaves: `pointDefenceRange` becomes
`comp::PointDefence{range}`, and `SpecialSpec::hook` and
`ShipSpec::preProcess` are replaced by `ShipSpec::equip`, a one-shot builder
run at spawn. It is still a function pointer, but it runs once at
construction instead of every frame, and everything after it is passes over
components. That is what ends `ilwrathPreProcess` as a global hook: the
cloak machine becomes a pass that never names the Ilwrath.

**Signals were rejected on principle.** `entt::sigh` / `on_construct<Fired>`
is the obvious ECS-flavoured answer to the `spawn` function pointer and it
is wrong here. Listeners fire during the emplace and are unordered relative
to each other; this simulation's entire thesis is declared order, and the
replay digest is folded in walk order. A signal would make spawn ordering
implicit and connection order global.

**The `spawn` function pointer becomes a `ShotPattern` instead.** Both
existing weapons make four *data* decisions once `muzzlePosition` is
factored out — frame index (facing, or fixed), `ignoreSimilar`,
`inheritsVelocity`, shot count — so both collapse to a literal with no code.
The discipline that keeps `ShotPattern` from becoming the next
`pointDefenceRange`: **a field earns its place only when more than one
weapon reads it.** A value only one weapon reads is a component on the shot,
or the escape-hatch function. Some SC2 weapons genuinely compute at fire
time (Melnorme charge levels, the Umgah cone, the Androsynth blazer) and
keep a function; the change is that the function stops being the only
mechanism.

Whatever replaces `SpawnFn` keeps its purity — const view in, values out, no
non-const path to the ship. That is a deliberate defence against a real C
bug (`umgah.c:330-341`, with `orz.c:249-253` compensating), not an
accident of style.

## 5. The one thing that is not bit-exact, and it is a spike

Extracting point defence into a true per-mechanic pass reorders
`spawnCommands_` — today the ordered walk interleaves `fire` and `special`
per ship, and separate passes would not. That moves `seq`, the walk, and the
digest.

Review-008 §6 measured this class of reorder and found every observable
outcome identical: 32/32 winners agreeing, 0 summed crew difference,
identical shot and collision counts. So the expectation is that the cost is
one re-record and nothing else. The expectation is not the evidence, so:
**after W5, extract point defence as a throwaway spike and measure it with
`--similar`.** If outcomes agree, the batch shape is free and W6 takes it.
If they move, that is a real finding about collision density with point
defence in play, and W6 keeps dispatch inside the existing ordered walks.

Either way the decision comes from the harness rather than from an argument.

## 6. Carried, not addressed

- A batched death pass, moving `runDeathResponses` off its mid-Collide call
  site. Needs a re-record, and W4 does not depend on it.
- Weapon archetypes beyond `ShotPattern`. Designing for 25 ships from a
  sample of two is guessing; the shape will be visible after ship three.
- `docs/README.md` does not exist, though `README.md` and `CLAUDE.md` both
  open by pointing at it, and three docs still cite `src/docs/` paths the
  migration moved.
