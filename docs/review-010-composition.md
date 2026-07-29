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
| `comp::matter` | `Physique`, `Collider`, `CollisionScratch`, `Planet`, `StashedMask`, `PriorSilhouette` |
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

`view` ×4, `context` ×4, `attach`, `attachOrReplace`, `get` and `detach` are
pure pass-throughs. `find` and `has` add a `valid(id)` check — real, since
`try_get` on a stale handle is undefined — but most call sites already open
with `if (!b.alive(id)) return;`. `eachOrdered` is the only wrapper with a
mechanism behind it: the sort, plus eliding `Order` from the callback so 36
call sites need no edit.

`ordered<Ts...>()` turned out to have **zero callers** — review-008 V2 added
it as the view form of the same mechanism and nothing ever wanted it. It is
deleted rather than kept for symmetry.

So: `entt::registry reg` becomes a public member, every pass-through is
deleted, and what stays is what has semantics — `eachOrdered`, the `spawn`
family, `alive`, `collidable`, the spawn queue, `step`, `rng`.

`ship()` and `weaponSpec()` stay, which is a line worth stating rather than
leaving as an omission. They are named accessors for one component each,
not registry vocabulary: keeping them gates access to no entt API, and
`weaponSpec` handles the absent-`FromWeapon` case its two callers would
otherwise repeat. If they should go too, it is a one-line follow-up.

Two changes make that safe rather than merely permitted:

- `count_` is deleted; `size()` reads `reg.view<comp::Order>().size()`, which
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

### What W2 found

Deleting `find`/`has` removes a `valid(id)` check that two app sites were
silently relying on, and **both are live paths, not hypotheticals.**
`MatchState::shipIds` outlives its ships — the wreck is reaped once it
finishes burning — so `Game.cpp`'s per-step input write and `Sound.cpp`'s
death-stinger check were both reading a stale handle through the wrapper's
guard. Under the wrapper they read as false/null; direct, they are
undefined. Both now ask `alive()` first, which says the thing the wrapper
was hiding.

Nothing else in the tree stores an entity id across frames: `Mark` holds a
`CollisionEvent` but never looks its ids up, `Allegiance::owner` and
`DamageIncoming::lastFrom` are compared and stored but never dereferenced,
and every other lookup runs on an id from the walk it is inside.

The suite did not catch either one — `sim_test` and `replay_test` never
build a `MatchState` — which is worth recording: the replay baseline proves
the *simulation*, and says nothing about the app above it.

## 4. Stages

| Stage | What | Proof |
| --- | --- | --- |
| W1 | `comp::` with inline groups; the `comp` macro deleted; `melee::comp` and `melee::ctx` | **done, bit-green** — 28 files, all 32 battles matched exactly |
| W2 | The entt surface goes: public `reg`, pass-throughs deleted, `count_` and `orderDirty_` derived | **done, bit-green** — 17 member templates deleted, ~430 call sites, two real defects found (below) |
| W3 | `lifeSpanOf` says what it means: both dead clauses deleted, `framesLeft` asserts, `isFiniteLife` becomes `isTransient` | **done, bit-green** — both claims proved by assert over the whole suite before deletion |
| W4 | `comp::Asteroid{mask, phase}` replaces `DeathSpawn` and `StashedMask` | **done, bit-green** — three tests moved off the generic payload onto the real mechanic |
| W5 | Spawns are built, not described: eager creation with `Order` withheld; `SpawnCommand` deleted | **done, bit-green** — six passes joined `Order`, one escape hatch left |
| W6 | Specials: `SpecialSpec` is the gate only; `PointDefence` and `Cloak` are components; `preProcess` and `hook` deleted | **done, bit-green** — the pre-turn slot is a per-mechanic pass; the gated one needs a re-record, so it is a follow-up |
| W7 | `Lifetime` stops being a counter: `{born, span}`, `ageDecrementPass` deleted | **blocked, priced** — not bit-exact after all; see below |

W1 and W2 sweep the same files and land together, as two commits.

### W3 — what stays

`Lifetime` and `Doomed` keep their names; they are not the defect, and the
component map's state table still holds. What changes is the function that
lied about them. `lifeSpanOf` served three different questions — remaining,
age, and is-persistent — and encoded the third as the magic `1`. After the
two dead clauses go, its four survivors all run on entities known to hold a
`Lifetime`, so it becomes `framesLeft`, which asserts. Debug builds are the
suite's assertion coverage.

One detail is load-bearing: `replay_test.cpp` folds this for **every**
entity, including those with no `Lifetime`, where it returned 1. That
expression is now spelled out in the test as `foldedLifeSpan`, at its one
remaining caller — moving it into `sim/` would be re-creating the thing this
stage deleted, and changing the value would move the baseline for a reason
unrelated to the change.

`lifeSpanOf` split three ways rather than two. `framesLeft` asserts;
`isTransient` answers presence; and `ageOf(b, id, span)` names what six sites
were spelling as `SPAN - lifeSpanOf(...)` — the explosion's spark schedule,
the guided shot's acceleration, and the ion-trail, warp-shadow and debris
ramps. Age was the third question the one function was answering, and the
subtraction order is a thing to get wrong exactly once.

**How the two claims were proved.** Not by argument alone: both conditions
were first asserted rather than deleted, and the whole suite plus all 32
battles ran with them live in a Debug build. Neither fired. The deletions
followed.

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

### What W5 found

**The deferral is a property of the moment, not of the call site.** `make()`
reads one flag: inside `step()` the Order is withheld, outside it lands at
once. Every existing spawn site was then correct without saying which it
wanted -- setup spawns land immediately because placement has to see what it
must not overlap, mid-frame spawns wait. The flag is cleared *before* the
landing runs, so the one deferred construction takes its Order in queue
order like everything else.

**`resolveAgainst`'s invariant needed restating, not fixing.** Its comment
said "nothing spawns or is destroyed mid-Collide, so neither pool moves
under these references" -- and after W5 things do spawn there (the blast,
the overlap-kill's rubble). The references survive anyway, for a reason the
comment did not give: entt pages component storage for pointer stability on
insertion, so only a *destroy* moves an element, and nothing is destroyed
mid-Collide. The comment says that now.

**`recordSpawn` moved to the landing**, which is also the moment every
component is attached -- so the flavor it derives reads a finished entity
rather than one mid-construction. That deleted `spawn()`'s `warhead`
parameter, which existed only to get a `Warhead` attached before the record.

**One test had to be rebuilt rather than ported.**
`testSpawnLandsAtSyncAndActsNextFrame` emitted from outside a step, which
now lands immediately by design, so the property it named could not be
observed that way. The replacement drives the asteroid cycle -- which emits
from slot 2 -- and probes with the rubble's own countdown: AgeDecrement runs
at slot 11 of the same step, so an untouched counter is proof the entity was
not in the walk. That is a stricter test than the position check it
replaced, which a stationary child would have passed vacuously.

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

### W7 — the clock census, and why `Lifetime` is the one to change

W3 named `framesLeft` and `ageOf` but left the mechanism alone. A census of
every counter in the sim says the mechanism is where the remaining problem
is.

**Seven wait gates** — `turnWait`, `thrustWait`, `weaponCounter`,
`specialCounter`, `energyCounter`, `Guided::clock`, `Spin::countdown` — all
"count down, act at zero, reload", in **two different policies**:

```cpp
if (c > 0) --c; else if (cond) { act(); c = period; }   // six sites
if (c > 0) --c;  if (c == 0 && cond) act();             // gateSpecial alone
```

`gateSpecial`'s own comment says why it differs — an else-branch "adds a
dead frame every cycle" (`ship.c:342-346`). The distinction is gameplay and
it lives **only in prose**. `Impulse` does a third thing: it raises
`turnWait`/`thrustWait` to a floor rather than arming them, which is the
collision stagger.

**One lifetime countdown**, `Lifetime::remaining`: the only pool decremented
for every member every frame, and the only clock whose death detection is an
**equality test on zero**. `Battle.cpp` names two tests that failed when it
did not land on exactly 0, which is why `ageDecrementPass` has to exclude
`Doomed`.

**Two animation counters**, and `AnimFrame` is two different things — an age
for `FrameDriven` flames, the raw facing for guided nukes. It unifies with
nothing, which is what review-007's overlap 4 already found.

So: **`Lifetime{born, span}`**, both stamped at attach.

```
framesLeft = born + span - frame
age        = frame - born
attach     = {frame, n}      // re-stamped; warp-in and explosion both restart it
kill now   = span = age
```

- `ageDecrementPass` deletes — one of sixteen slots, and the only pass that
  touches every transient every frame.
- The equality-on-zero landmine becomes `age >= span`, which cannot be
  stepped past. The `Doomed` exclusion existed only to protect that
  equality, and goes with it.
- `ageOf(b, id, span)` loses its parameter, so the six sites W3 left passing
  a constant can no longer pass the wrong one.
- `Lifetime` becomes immutable after attach, so `WeaponSpec::lifetime` --
  already a `Lifetime` copied verbatim into the shot -- stops meaning
  something subtly different in the spec than on the entity.

**It was expected to be bit-exact. It is not, and the reason is worth
having.**

`ageDecrementPass` is slot 11 of eighteen, so `remaining` **changes value
in the middle of a frame**. Readers before it and readers after it see
different numbers for the same entity in the same step:

| Reads `remaining` | Slot | Sees |
| --- | --- | --- |
| `ageAndReapMarkPass` | 2 | pre-decrement |
| `warpInStep`, `explosionStep` (via ShipMachines) | 4 | pre-decrement |
| `guidedShotPreProcess` | 7 | pre-decrement |
| `resolveAgainst` | 12 | **post-decrement** |
| `Draw`, the replay digest | after the step | post-decrement |

A value derived from the frame counter is constant across the step by
construction, so no single `{born, span}` convention reproduces both
columns. Shifting `born` by one trades which column matches.

Worse, **the attach sites are themselves in different phases**, so the same
nominal span already means different things:

- `warpInStep` attaches `Lifetime{WarpingIn::kFrames}` at slot 4, *before*
  the decrement, so the arriving ship is aged on its own attach frame.
- `startShipExplosion` attaches `Lifetime{Exploding::kLife}` from
  `applyDamageIncoming` at slot 14, *after* it, so the wreck is not.
- Every spawned transient lands at slot 17, later still.

So today's nominal span is not the effective one, and by how much depends on
which slot did the attaching. A uniform rule cannot reproduce that, and
should not want to -- **the inconsistency is the thing W7 would fix**, not
an invariant to preserve.

The price is therefore one baseline re-record, and unlike W6's that is not
merely a walk-order reshuffle: it changes a life by a frame at one attach
site. The outcome spread should still be small, and `--similar` would say,
but this one deserves the question asked rather than assumed. **W7 is
implemented nowhere and blocked on that.**

The rest of the design stands unchanged and is worth keeping on the shelf:
`ageDecrementPass` deletes, the equality-on-zero landmine becomes
`age >= span`, `ageOf` loses its span parameter, and `Lifetime` becomes
immutable after attach. `WeaponSpec::lifetime` would become a plain `i32`
in the same move, since a `{born, span}` value has no meaning sitting in a
spec.

**The seven gates are not part of W7.** A `Countdown` type with
`openElseTick`/`tickThenOpen`/`raiseTo` would turn that prose distinction
into a declared one, and it is bit-exact -- but it is seven sites of four
lines and the call sites do not obviously read better afterwards. Worth
doing only if the diff comes out small. Grouping the ship's counters into
structs stays rejected: review-009 disqualified `Stagger{turn, thrust}` and
`Cooldowns{weapon, special}` because nothing operates on either pair, and a
type *per counter* is a different proposal from a struct *per pair*.

W7 goes after W5, which rewrites every spawn site -- and those are exactly
the sites that attach `Lifetime`.

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

### Measured, 2026-07-29 — free, and deliberately not taken yet

The spike was the cheapest form of the reorder: `fireAndSpecialGatePass`
split into fire-for-every-ship then special-for-every-ship, which is exactly
the interleaving per-mechanic passes produce, with nothing else changed.

| Measure | Baseline | Split passes |
| --- | --- | --- |
| winner | — | **32/32 agree** |
| crew lost, summed \|diff\| | 734 total | **0** |
| shots | 24910 | **24910** |
| collisions | 399 | **399** |
| end frame, median \|diff\| | median 2129.5 | **0.0** |

Identical, not similar — the same result review-008 §6 got for walk order,
and for the same reason: 399 contacts across 32 battles of ~2130 frames is
about twelve per battle, so within-frame ordering almost never has two
interactions to sequence.

The digest moved in 25 of 32 battles, and only from frame 705 in the sample
— unlike the walk reversal, which diverged before frame 64 in all 32. The
reorder only bites once a special actually fires.

**So the batch shape costs one re-record and nothing observable. W6 did not
take it.** The standing instruction is not to re-record without being asked,
and the measurement does not repeal it: it establishes the price, not the
permission. W6 therefore keeps the dispatch inside the existing ordered
walks, which is bit-exact, and the per-mechanic passes are a small
follow-up whenever that is authorised — the mechanics are already components
with their own step functions, so it is a change to where they are called
from and nothing else.

### Measured again, same day — the *pre-turn* slot is free, and is taken

The price above is the gated slot's. The pre-turn slot was never measured,
and it turns out to cost nothing: `preTurnSpecialsPass` is bit-green, so the
cloak is a per-mechanic pass now rather than a per-entity dispatch, and
`runPreTurnSpecials` is gone.

The catch is *where* the pass goes, and it is not the walk-order question
this tree usually asks. Run **after** `shipMachinesPass` it diverges in 10
of 32 battles before frame 64, flipping at least three winners. The trace
localises it to a single number: battle 6, frame 15, the arriving Ilwrath
holds 12 energy instead of 15 — one cloak activation, three energy, spent a
frame early.

Frame 15 is `WarpingIn::kFrames`, and `warpInStep` **removes `WarpingIn` on
the arrival frame**. Interleaved, the `return` after `warpInStep` spends
that ship's frame. As a separate pass, `entt::exclude<WarpingIn>` is
evaluated after the tag it excludes on has already been erased, so the
arriving ship takes a step the early return had denied it.

**A sequenced early return is not a query predicate.** Transposing
`for ship { if A return; if B return; mechanic }` into
`view<Mechanic>(exclude<A, B>)` bets that nothing between the two evaluation
points mutates `A` or `B` — and here the pass mutates its own guards. That
is the check every future mechanic owes, and it is a different question from
the walk-order one review-008 §6 settled.

Run **before** `shipMachinesPass` the bet holds and the baseline does not
move. For a live ship the placement is a no-op — the cloak call was all that
remained of `shipMachinesStep` — so the only couplings left are spawn order
and the RNG stream. The cloak spawns nothing, and it draws only on
`trackShip`'s exactly-astern coin flip; in 1v1 that draw cannot race
`explosionStep`'s, because a dead enemy leaves `trackShip` with no target
and so no draw at all. **In a three-or-more-ship melee that argument is
gone**, so the placement is bit-exact by measurement here and by
construction nowhere. It is the first thing to re-measure when melee grows a
third ship.

### What W6 landed

`SpecialSpec` keeps the uniform half and nothing else: a cost and a
debounce, which is all the engine ticks (`ship.c:342-346`).
`pointDefenceRange` is `comp::PointDefence{range}`, and `SpecialSpec::hook`
and `ShipSpec::preProcess` are gone.

The effects moved out of the ships entirely. `ilwrathPreProcess` became
`cloakStep`, a mechanic over `comp::Cloak` that never names the Ilwrath;
`cruiserSpecial` became `pointDefenceStep` over `comp::PointDefence`. A ship
now *composes* mechanics rather than owning them:

```cpp
.equip = [](Battle &b, EntityId id) noexcept {
    b.reg.emplace<comp::Cloak>(id);
},
```

`equip` is still a function pointer, but a one-shot builder run at spawn
rather than a hook run every frame — everything after it is a step over
components. A ship reusing a mechanic costs nothing outside its own file; a
*new* mechanic costs a component, a step, and one query (pre-turn) or one
line (gated) in the slot that `sim/Specials.cpp` owns. Adding a ship never
edits the core.

One behavioural detail: `Cloak` is attached at spawn now rather than lazily
on first use, because presence is what dispatch keys on. `Cloak{0}` is the
solid state and every reader already treats it as such, including Draw's
tint ramp, which starts at level 1.

## 6. Carried, not addressed

- A batched death pass, moving `runDeathResponses` off its mid-Collide call
  site. Needs a re-record, and W4 does not depend on it.
- Weapon archetypes beyond `ShotPattern`. Designing for 25 ships from a
  sample of two is guessing; the shape will be visible after ship three.
- `docs/README.md` does not exist, though `README.md` and `CLAUDE.md` both
  open by pointing at it, and three docs still cite `src/docs/` paths the
  migration moved.
