# Worknotes

A running log of open threads: things noticed in passing that are not yet a
decision, not yet a bug, and would otherwise be lost between reviews.
`design-notes.md` holds settled rationale; the numbered reviews hold finished
arguments. This holds the ones still moving. Newest first, dated.

## 2026-07-29 — from the review of #4 (`sim/spawns-are-built`)

### Why the sync point is not about visibility

W5's spawn rework reads as a visibility device — a mid-frame spawn must not
be seen, collided with or steered toward. That is true and it is the weaker
half. The constraint that actually forces a sync point is:

> **The `Order` pool must not be mutated between the top of `step()` and the
> landing.**

`ensureOrdered()` sorts that pool, `on_construct<Order>` raises
`orderDirty_`, and `eachOrdered` **nests**. Two nestings exist today:

| outer walk | reaches | inner walk |
| --- | --- | --- |
| `ageAndReapMarkPass` | `runDeathResponses` → `sweepDeadShipOrdnance` | `eachOrdered<Allegiance>` |
| `collidePass` | `resolveAgainst` → `killOverlapSpawn` → `runDeathResponses` | the same |
| `fireAndSpecialGatePass` | `gateSpecial` → point defence | `eachOrdered<Physique, Position>` |

So a mid-pass `emplace<Order>` would have the *inner* `ensureOrdered()`
permute the packed array the *outer* walk is holding an iterator into. That
is the thing a "not yet visible" tag on the entity cannot give: excluding a
newborn from views says nothing about whether the pool moved.

Two smaller costs on the tag design, for the record: `size()` is
`reg.view<Order>().size()` and would start counting unlanded entities (a view
with an exclusion has no `O(1)` `size()`), and an append breaks `(layer, seq)`
sortedness immediately — a Background ion trail after an Ordnance shot — so
the re-sort has to happen somewhere regardless. The queue would shrink to a
dirty flag; the sync point would stay.

### The ordering keeps setting the terms of unrelated changes

That is now the third time. After the walk-reversal measurement
(`review-008` §6) and the batch-shape re-record W6 declined (`review-010`
§5), it is what made spawn *creation* need a sync point. `seq` is documented
as "determinism, not gameplay" — a free parameter — but every structure built
over it acquires a constraint that reads as fundamental and is not. Each is
individually cheap to state; collectively they are why the walk cannot be
reordered, specials cannot move to per-mechanic passes, and an entity cannot
be constructed wherever it is wanted.

Worth asking, before the next thing leans on it, whether the ordered walk
should be a **derived index rebuilt at the sync point** rather than a sorted
component pool that any construction can dirty. That decouples "what order do
we walk" from "may I create an entity right now", which is the coupling that
keeps surfacing.

### `eachOrdered` is not called from an `on_construct` handler — checked

The only observers are `markOrderDirty` on construct and destroy of `Order`
(`Battle.cpp:111-112`), and both do nothing but set `orderDirty_`. The hazard
runs the other way: a construct raises the flag, and a *later* nested
`eachOrdered` performs the sort. Three nesting paths exist (table above),
which is more surface than a first read suggests — a fourth in a death or
collision response is what would turn the flag into a live iterator bug.

### Still not convinced `PendingSpawn::deferred` is strictly necessary

`spawnAsteroid` (`Field.cpp:147-192`) reads nothing about the world — no
overlap avoidance, no placement query. It draws seven RNG values and puts a
rock on an arena edge. So the escape hatch is not buying correctness; it is
buying *stream position*. Other mid-frame consumers exist
(`ShipSystems.cpp:577,587`, the explosion debris drift; `Targeting.cpp:82`,
the AI tie-break turn), so calling `spawnAsteroid` at emission would
interleave its draws against those differently.

Which makes this the same shape as the two open items in `CLAUDE.md`:
**dropping `deferred` looks like it costs one baseline re-record and nothing
else.** If that measures out, `queueDeferred`, `PendingSpawn::deferred` /
`deferredMask` and the branch in `landPendingSpawns` all go, and `pending_`
becomes a plain list of ids and layers. Not yet measured — the check is to
call `spawnAsteroid` directly from `advanceAsteroidCycle` and see whether the
divergence is a stream offset (expected) or a behavioural change
(unexpected, and then the comment is right and should say why).

## 2026-07-29 — from the review of #5 (`sim/specials-are-components`)

### The closed dispatcher costs the gate test its isolation

`runGatedSpecials` is `if (has<PointDefence>) pointDefenceStep(...)`, so
nothing outside `Specials.cpp` can register a mechanic. The old gate-timing
test installed a counting `SpecialSpec::hook` and observed `gateSpecial`
alone; the replacement drives real point defence and counts `Laser`
`SpawnEvent`s, which makes it depend on a target with a `Collider` inside 100
display px, `Vitality{50}` outlasting four volleys, `isGravityMass(1)` being
false, the target not being `Cloaked`, and `energyCost = 0`. Six ways to fail
a test about one decrement.

Not a defect — but the PR frames the dispatcher line as the whole cost of a
new mechanic, and this is the other half of it. If mechanics ever need to be
injectable again, that is the reason.
