# Review 008 — the entt surface: views escape, the order sorts itself

SiGMan's ask: `Battle`'s wrappers hide entt's power and force us to invent
our own spellings for iteration and filtering that entt already has. The
strict-ordering assumption costs an explicitly built index
(`buildOrderedIds`, and `fetchOrdered` on top of it) and leaves walks that
iterate bare ids — which should be rare in an ECS. entt has its own `sort`.
And if that lands, can `Order` go, leaving only `Layer` to filter
background elements?

Three of those four hold. The fourth — dropping `Order` — fails for a
mechanical reason worth stating precisely, because the instinct behind it
is right and only the last step is wrong.

The bar is unchanged from review-007: **bit-green, the replay baseline
never moves.** That gate is unusually well-suited to this review, since the
digest folds the walk order every frame — any ordering mistake is a
divergence on the first battle, not a subtle regression.

## 1. What was measured

| Fact | Value |
| --- | --- |
| `eachOrdered` call sites | 39 (src + tests) |
| `each<Ts...>` call sites | 14 |
| `find<>` in `src/sim` | 82 (`src/app` 5, tests 134) |
| Walks that iterate bare ids, production | 4 — `ageAndReapMarkPass`, `calculateGravity`, two in `ShipSystems` |
| Allocation per `eachOrdered` call | one `std::vector<EntityId>`, both overloads |

The sharp one: `calculateGravity` runs a bare `eachOrdered` **and is itself
called per entity**, from `gravityPass` and from Field's placement loops.
That is a heap allocation and an O(n log n) sort nested inside an O(n²)
walk. `collidePass` is the only ordered walk that reuses its buffer.

`fetchOrdered` is the other tell. It exists only because `eachOrdered`
destructures by hand, and it re-implements what entt's own views already
do with empty types. A wrapper that needs a wrapper is the signal that the
wrapping itself is the problem.

## 2. What entt already offers

- `registry::sort<T>(cmp)` sorts a pool in place. It asserts on pools with
  tombstones ("Sorting with tombstones not allowed"), which does not bind
  us: `Order` is a plain component with swap-and-pop storage.
- `view::use<T>()` forces a view to iterate driven by a chosen pool. A
  sorted `Order` pool plus `use<Order>()` is ordered iteration, natively,
  with no scratch and no hand-destructuring.
- Views also carry iterators, `size_hint`, storage access and groups —
  none of which reach a call site through the current `each<Ts...>` shape.

## 3. Why `seq` stays

The proposal was to keep `Layer` and let the registry's own order supply
the rest. It cannot, and the reason is the same one that makes entt's
canonical example work:

    registry.sort<renderable>([](auto &lhs, auto &rhs) { return lhs.z < rhs.z; });

That works because `z` *distinguishes* renderables. `Order{layer, seq}` is
our `renderable.z`, and `seq` is the half that distinguishes. Sorting by
`layer` alone is not a total order — every Field entity compares equal —
and entt's default `Sort` is `std_sort`, plain `std::sort`, which is
unstable: equal elements take arbitrary relative positions and may move on
every re-sort.

Substituting stability does not rescue it. `std::stable_sort` preserves
*current pool order*, and `Order`'s pool is swap-and-pop: a single destroy
moves the last element into the hole, so the order being preserved is
already scrambled. Nor can the entity id serve as a tiebreak — entt
recycles ids, so a reused slot would sort into a dead entity's position.

You cannot preserve what the key does not encode. `seq` is the key.

So `Layer` stays *inside* `Order` as the major component of the sort key
rather than becoming a standalone tag, and the win SiGMan was after — no
built index, no `fetchOrdered`, no per-call allocation — arrives anyway,
from sorting the pool once instead of rebuilding the order 39 times.

## 4. The stages

| Stage | What | Proof |
| --- | --- | --- |
| V1 | Views escape: `Battle::view<Ts...>()` (and the exclude form) returns entt's view; the `each<Ts...>` sites migrate to `view<...>().each(...)`; the `each` wrappers delete. The registry stays private | **done, bit-green** — 9 live callers, not the 14 estimated here (that count was occurrences of the pattern, including `Battle.cpp`'s own already-direct `reg_.view` calls) |
| V2 | The order sorts itself: `sort<Order>` behind a dirty flag set by spawn and destroy, so it runs about once per frame instead of once per walk; ordered walks become `view<...>().use<Order>()`. `buildOrderedIds` and `fetchOrdered` delete, and with them the per-call allocation. `collidePass` keeps its snapshot — see risks | **done, bit-green** — no call site changed, so the gate tested the mechanism alone. See the measurement below |
| V3 | The bare-id walks end: the four production sites take real joins. `calculateGravity` is the valuable one — the nested sort and its allocation leave the O(n²) path with it | **done, bit-green** — three of four; the fourth has a reason, below |
| V4 | `find<>` in `src/sim` pruned where it is a same-entity read that belongs in the join's signature; the survivors are conditional cross-entity reads, as the join rule already says | **done, bit-green** — 63 audited, 21 survive |

### What V3 and V4 found

**One bare-id walk stays, and it is right to.** `ageAndReapMarkPass` looked
like the easiest conversion of the four and is the one that cannot be made.
Its body calls `runDeathResponses`, whose `SweepsOwnedOnDeath` branch
reaches `sweepDeadShipOrdnance`, which does `attachOrReplace<Lifetime>` on
*other* entities — and entt puts adding to a pool the active view reads in
the undefined column. It is a live path, not a hypothetical: every ship
death attaches `Lifetime` and `SweepsOwnedOnDeath` together in
`startShipExplosion`. So the pass keeps `find<Lifetime>` over a bare walk.

The comment above it had argued the same conclusion from the wrong premise
— that a join would become "a pool view over Lifetime" and lose the
declared order. That part was untrue (`eachOrdered<Lifetime>` is the
ordered walk filtered, not a Lifetime-pool walk); the real reason is the
mid-walk mutation.

**The `find<>` audit found a different problem than the one it went
looking for.** The join rule targets same-entity reads inside a walk that
belong in the signature — there was exactly one of those. What the audit
actually turned up is that **43 of 63 sites were not in a walk at all** and
dereferenced their result with no null check: assertions written as
lookups. Those became `get<T>`, which asserts. The population had also
shrunk from the 82 quoted in §1, since V3's conversions took some with them.

21 survive, and each is what the rule permits: one genuinely optional
same-entity read (the flame's post-detonation linger frame has no
`Collider` but must keep advancing its `AnimFrame`), one conditional
cross-entity read, and single-entity reads that branch on absence rather
than skipping.

### What V2 cost and returned

Measured A/B on the same machine, same build, the only difference being
V2's commit stashed or applied:

| | replay suite | whole suite |
| --- | --- | --- |
| Before V2 | 50.48s | 56.46s |
| After V2 | 27.76s | 32.90s |

The replay suite runs 32 battles to completion, so it is the closest thing
to a sim benchmark this repo has. Nearly halving it confirms the diagnosis
rather than merely satisfying it: the cost was real, it was the per-call
rebuild, and the nested walk in `calculateGravity` was where it hurt.

Two things fell out of the implementation that the plan had not:

- The `const eachOrdered` overload had **no callers** — the only
  `const Battle &` contexts are `lifeSpanOf`, `isFiniteLife` and one test
  helper, none of which walk. It was deleted rather than supported, which
  is what keeps `reg_` non-`mutable`: sorting is a write, and a const walk
  would have forced the whole registry to be mutable to allow it. Losing
  const enforcement across the entire class to serve zero callers is a bad
  trade; if a const ordered walk is ever wanted, the question reopens then.
- `fetchOrdered`'s whole job — eliding empty components from what the
  callback receives — is something entt's `each` already does. Swallowing
  the leading `Order &` and taking `auto &...rest` reproduces every
  existing call site's lambda signature exactly, which is why 39 call sites
  needed no edit at all.

## 5. Risks

- **V1 reverses a recorded decision.** review-004's open question 3 settled
  on "`entt::registry` never escapes `Battle`", and this hands out views.
  The reversal is deliberate and narrow: views escape, the registry does
  not, so ownership-by-component-type is unaffected while the query
  vocabulary stops being ours to reinvent.
- **V2's sort point must cover every mutation.** A spawn into a non-maximal
  layer breaks sortedness even though `seq` only grows — a Background trail
  spawned late must sort ahead of a Field ship spawned first. A dirty flag
  set by both spawn and destroy is the conservative choice; the replay is
  the check.
- **`collidePass` keeps its snapshot.** It needs indexed successor-only
  access (`processCollisions(id, i, i + 1, …)`) and stability across a walk
  that attaches `Doomed` mid-flight. A live view gives neither. Converting
  it is a separate question, not part of V2.
- `(layer, seq)` is a total order because `seq` is unique, so `std::sort`'s
  instability is irrelevant *once the key is right* — the same fact that
  kills the `Layer`-only comparator makes the real one safe.

## 6. Open question — does the order matter to gameplay?

SiGMan doubts it, and the doubt is recorded here unresolved rather than
argued away: bit-exactness obviously depends on walk order, but that is a
property of the harness, not of the game.

The first version of this section overstated the case, and the correction
runs in SiGMan's favour. It claimed `resolveAgainst`'s second collision
response was conditional on surviving the first — that a kill could
suppress the other side's response, so who came first decided who lived.
That guard could never fire: nothing is destroyed during Collide, since
`removeElement`'s only caller is the reap at a later sync point. Both
responses always ran. The guard is gone now, along with every other
unreachable liveness test in that path.

What order actually decides is narrower. Walk position fixes which entity
is the scanner and which the test, and that feeds the
`all_of<ShipState>(testId)` branch which picks *response sequence* — and
sequence matters because a response mutates state the next one reads
(hit points, the Collided flag, a detached Collider). Separately, each
pair's resolution mutates positions and velocities that later pairs in the
same frame read. So outcomes are order-dependent, but through accumulated
sequencing rather than through one decisive kill.

That is a weaker claim than the one this section opened with, and it makes
the counter-argument stronger: most frames resolve no collisions at all,
the Pkunk phoenix is one unbuilt ship, and "different" may not mean "worse"
or even "noticeable".

### Measured, 2026-07-28 — SiGMan was right

Relaxing the comparator to layer-only would have been the obvious
experiment and the wrong one: that order is not deterministic (`std::sort`
is unstable over equal keys, and the pool underneath it reorders on every
destroy), so it would have measured noise. The experiment run instead keeps
a **total, deterministic** order and merely picks a different one — `seq`
descending within layer, layers still ascending. `--similar` against the
existing baseline, all 32 battles:

| Measure | Baseline | Reversed order |
| --- | --- | --- |
| winner | — | **32/32 agree** |
| crew lost, summed \|diff\| | 734 total | **0** |
| shots | 24910 | **24910** |
| collisions | 399 | **399** |
| end frame, median \|diff\| | median 2129.5 | **0.0** |

Every observable outcome is *identical*. Not similar — identical.

The digest, meanwhile, diverges in all 32 battles, before frame 64 in every
one. That is not a contradiction: the digest folds entity state **in walk
order**, so reversing the walk changes the hash by construction. The hash
was measuring the walk, not the state. The outcome columns are what measure
the state, and they did not move.

Why the sim is order-invariant here is visible in its own numbers: 399
collisions across 32 battles of ~2130 frames each is roughly twelve
contacts per battle, so within-frame ordering almost never has two
interactions to sequence. Where it does, `resolveAgainst`'s
`all_of<ShipState>(testId)` branch normalises ship-vs-anything response
order — the ship responds first whichever side it is on — and the momentum
exchange itself is symmetric.

**So within-layer FIFO is not load-bearing for gameplay.** The claim this
section opened with was wrong, and the prediction that replaced it — that
chaos would make outcomes diverge widely — was also wrong.

`seq` still stays, for the reason that survives: it is the tiebreak that
makes the sort **total and deterministic**. entt's pool order is not stable
under swap-and-pop deletion or tombstone reuse, so without it the walk
would vary run to run and the replay harness protecting every other change
would stop meaning anything. `layer` keeps its own separate justification —
declared strata, and the Pkunk phoenix that will need to preprocess ahead
of a dying ship.

The useful consequence: the walk order is now known to be a free parameter
for gameplay. Changing it costs one baseline re-record and nothing else.

One caveat on scope: this is the two-ship melee with the current roster. A
denser fight — more ships, or a weapon that fills the field with ordnance —
would raise the collision density that makes this result hold.
