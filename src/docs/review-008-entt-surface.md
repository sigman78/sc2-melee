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
| V3 | The bare-id walks end: the four production sites take real joins. `calculateGravity` is the valuable one — the nested sort and its allocation leave the O(n²) path with it | bit-green |
| V4 | `find<>` in `src/sim` pruned where it is a same-entity read that belongs in the join's signature; the survivors are conditional cross-entity reads, as the join rule already says | bit-green; the survivor list is the record |

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
property of the harness, not of the game. The claim that *gameplay* depends
on it rests on `resolveAgainst` (`Battle.cpp:554`), where collision
response is sequential and asymmetric —

    if (all_of<ShipState>(testId)) { respond(testId, elemId);
                                     if (alive(elemId)) respond(elemId, testId); }
    else                           { respond(elemId, testId);
                                     if (alive(testId)) respond(testId, elemId); }

— so the second response is conditional on surviving the first, and which
entity is the scanner is decided purely by walk position. Two missiles
meeting head-on resolve differently depending on which came first.
`testOpposingMissilesDestroyEachOther` is the existing pin.

The counter-argument is that this is a narrow case: most frames resolve no
collisions at all, the Pkunk phoenix is one unbuilt ship, and "different"
may not mean "worse" or even "noticeable".

**This is measurable, and should be measured rather than debated.** The
replay harness already records per battle: winner, end frame, crew lost per
side, shots and collisions. Relaxing the comparator to layer-only and
running the 32 battles reports exactly how many change outcome, and by how
much. A result of "2 of 32 change winner" and one of "19 of 32" argue for
different designs. Until that runs, `seq` stays — not because the gameplay
claim is proven, but because the ordering is load-bearing for the gate that
protects every other change in flight.
