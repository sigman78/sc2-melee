# Review 004 — the EnTT experiment

SiGMan's ask: adopt a lightweight ECS library on a branch, exemplify how the
architecture changes, and report honestly — where the wins are, and which
parts come out more complicated than they should be. This deliberately
reopens the storage decision sim-architecture.md closed ("archetype storage:
adopted not at all") — that document names itself as the place the argument
gets reopened, and this branch is the reopening, run as a falsification
experiment rather than a refactor that quietly assumes its conclusion.

**The oracle**: the existing test suite. sim_test pins the collision
protocol, spawn ordering and RNG-exact battles; a storage swap that
preserves walk order and draw order keeps them green, which is *stronger*
than the agreed bar (gameplay the same within ~1-frame differences). Where
a test pins the old storage API rather than gameplay, the test changes and
the commit says so.

## 1. The library: EnTT v3.16.0, not flecs

Both were considered against what this sim actually is: ~40 entities,
24 Hz, single-threaded, deterministic, ordered-walk gameplay, WASM target.

**EnTT** (skypjack/entt v3.16.0, Nov 2025): header-only C++20 (we are
C++23), zero dependencies, Emscripten-clean, and it lands exactly where
cmake/Dependencies.cmake:1-7 says every dependency lands — FetchContent
with FIND_PACKAGE_ARGS and an `uqm::entt` INTERFACE alias, PUBLIC on
uqm2_content since registry types will appear in sim/ headers. Its
model — independent sparse-set pools per component type, entity as a
versioned integer id, a registry that is *storage only* — is our existing
architecture industrialized: `Battle`'s component sidecars are hand-rolled
sparse sets already, and `EntityId{index, generation}` is `entt::entity`'s
id+version spelled as a struct. EnTT imposes no scheduler, no pipeline, no
world model; iteration order is ours to own, which is the one property the
sim cannot give up. Pointer stability is opt-in per component
(`in_place_delete`), sorting is available but optional.

**flecs**: archetype/table storage — entities physically *move* between
tables when a component is added or removed, which is the worst possible
match for "traversal order is gameplay" and for the C's habit of toggling
per-entity state mid-frame. It also arrives with its own world, pipeline,
scheduler and query DSL: adopting flecs is adopting flecs's architecture,
not seeing ours reshaped by a component store. A fine library, the wrong
experiment.

## 2. What the architecture becomes

The mapping, piece by piece. `Battle`'s public surface (spawnFront/Back,
step, get, ship, events, rng) survives almost unchanged — it is a clean
seam, which is what makes a staged migration possible.

| Today | Under EnTT | The change in kind |
| --- | --- | --- |
| `EntityId{index, generation}` | `entt::entity` (id+version in one uint32) | mechanical but wide: EntityId rides by value in CollisionEvent/SpawnEvent, the app's Game::ships and marks, and every hook signature; `kNoEntity` → `entt::null`, `.valid()` → a free helper |
| `EntityList<Element>` — chunked slot arena + intrusive doubly-linked order | `entt::registry` for storage + an `OrderLink{prev, next}` component and head/tail in Battle | **the load-bearing move**: the C's `disp_q` becomes a component; the registry does not own order, the spine does |
| `EntityRef` checked borrows (debug stale-deref diagnostics) | `registry.valid()` + `try_get` | a real loss — the epoch diagnostic dies; ledger item |
| `Element` (fat universal core) | stage X2: one `Element` component, unchanged; stage X5: split by concern | decomposition is the experiment, not the migration |
| `ships_`/`weaponSpecs_` sidecar vectors + attach/drop/find plumbing | pools: `registry.emplace<ShipState>`, `try_get`, auto-destroyed | pure deletion — this is what a registry is |
| `ElementFlags` bitfield (13 bits, "close to full", review-002) | lifetime traits become tag components (`BeamGeometry`, `PlayerShip`, `IgnoreVelocity`); per-frame protocol bits stay a small bitfield; **`Cloaked` stays a flag by decision** — it projects the cloak machine's state, and the census's Cloak *component* is its destination; a tag beside that state would be the stored-vs-derived sync hazard (review-002's POINT_DEFENSE case) | the bit-exhaustion problem dissolves; lifecycle bits deliberately do NOT become tags (add/remove churn per frame is noise, not signal) |
| App `RenderStore` + `purgeDead` | `Visual` as a component in the same registry | `purgeDead` deletes itself: destroying an entity destroys its Visual. Ownership by component type, not by store |
| Hooks `void(*)(Battle&, EntityId)` | unchanged | deliberate: every ship was tuned against direct dispatch (sim-architecture.md); the registry hides behind Battle |

### The one thing no ECS provides

The step loop is a LIVE walk under mutation: hooks spawn elements
mid-pass and the walk reaches them the same frame (tail spawns caught up,
head-inserts preprocessed before their inserter's successor); the reap
removes mid-walk. Pool iteration — EnTT views included — cannot express
"walk into what was just appended, in insertion position". So the linked
order stays, as data: `OrderLink` with `in_place_delete`, walked exactly
as `EntityList::next` is today. And the order dependency reaches further
than the step loop: `trackShip` breaks distance ties by list position
(Ship.cpp:283-318, strict `<`), `cruiserSpecial` pays energy at the
*first* in-range target and aborts the volley if the spend fails
(Ship.cpp:631-670), `calculateGravity` early-outs on the first source
(Gravity.cpp:61), and the app's draw loop *is* the z-order
(Draw.cpp:339). Views enter only where the result is provably
order-independent (the boolean any-overlap scans, Field.cpp:74-93,
111-123); every other pass walks the spine. If the experiment ends with
the spine intact — and it will — that is not a failure of the
experiment, it is its most quotable finding.

### Where the wins should appear

1. **Sidecar plumbing deleted** — `attachShip`/`dropComponents`/linear
   finds become one-liners; every future M2 component (limpets, charge
   state, marine riders — the census's field-repurposing evidence) is
   `emplace<T>` instead of a new sidecar vector + lifecycle code.
2. **Flag bits become types** — `any(flags & ElementFlags::Cloaked)`
   becomes `all_of<Cloaked>`; bit 15 never arrives.
3. **The app side collapses** — RenderStore, its find loop and purgeDead
   go; Visual rides the entity.
4. **~350 lines of EntityList/EntityRef retire** in exchange for a
   dependency that is tested by half the industry.

### Where the friction should appear

1. **The ordered walk** must be reimplemented over OrderLink — the code
   exists (EntityList), the risk is subtle order bugs the suite exists to
   catch. Two facts make it tractable: removal is confined to a single
   site (the reap, Battle.cpp:580, always immediately after reading
   `next`), and mid-frame spawns land only at head or tail plus one
   as-yet-unused insertAfter.
2. **Pointer discipline changes shape — and the sidecars, not the
   elements, are the live hazard.** Element* is arena-stable today
   (pinned by testEntityAddressesAreStable, sim_test.cpp:198-230, which
   names the bug it caught), and the hook code leans on it:
   shipPostProcess holds a ShipState& across the weapon-spawn loop's
   spawnBack/attachWeaponSpec (Ship.cpp:139-249), shipPreProcess across
   the ship hook and spawnIonTrail (Ship.cpp:59-132). That survives
   today only because nothing attaches a ShipState mid-step. EnTT's
   default swap-and-pop storage would break such references on *any*
   removal from the pool; `in_place_delete` on Element and ShipState
   (plus a reserve) removes the hazard class, at the cost of tombstone
   iteration — irrelevant at 40 entities, but a decision every component
   now has to make explicitly. The epoch diagnostic (EntityRef's
   removed-while-held report) has no EnTT equivalent; debug builds lose
   a real safety net.
3. **Registry access verbosity** — `b.get(id)->x` becomes
   `reg.get<Element>(id).x`; fine until X5 splits Element, at which point
   every hook says what it touches. That is either self-documentation or
   ceremony; the ledger decides after Ship.cpp is converted.
4. **Compile time** — header-only template machinery in every sim TU.
   Measured before/after.

## 3. The staged plan

Every stage ends with the full suite green and a commit; sonnet subagents
do the mechanical sweeps, the design stays in the main session.

| Stage | What | Proof |
| --- | --- | --- |
| X1 | Vendor EnTT v3.16.0 by FetchContent beside SDL; a smoke TU instantiates a registry in uqm2_content | it builds, everywhere CI builds |
| X2 | The storage swap: `EntityId` = `entt::entity`, registry + `OrderLink` replace EntityList; `Element` rides whole as one component; Battle's surface unchanged; EntityList/EntityRef deleted | **done** — every gameplay pin bit-green; the five arena-pinning tests rewritten against the spine (order, reap-during-walk, stale handles, slot reuse) or re-pointed at what now provides the guarantee (address stability pins in_place_delete); entity zero is a live entity in EnTT, so every default-constructed EntityId became an explicit kNoEntity, surfacing one latent uninitialized-enum bug in the tests |
| X3 | Sidecars → pools (`ShipState`, weapon spec); app `Visual` becomes a component, RenderStore and purgeDead deleted | **done** — suite green, driven run; one new truth surfaced: an element executed for spawning inside something is destroyed before its SpawnEvent is read, so the app guards the Visual attach with alive() where the old store appended blindly and purged later |
| X4 | Trait flags → tag components (three of the four: Cloaked stays, see §2); order-sensitive scans stay on the spine | **done** — suite green, driven run; friction collected: tags attach to the id, not the Element value, so every spawn site splits into spawn-then-emplace; and applyImpulse, pure over two Elements, is now *told* aIsShip/bIsShip by the caller — a trait that leaves the struct must be handed to pure functions as a parameter |
| X5 | Split `Element` by concern | **done, as two worked splits and a declination** — `Cloak{level}` (only the Avenger's machine emplaces it; `isCloaked()` derives OBJECT_CLOAKED from it, stored nowhere) and `PriorSilhouette` (the overlap-repair scratch, a component in Battle.cpp's anonymous namespace — a type nothing else can name). The universal core stays one `Element`, deliberately: see the verdict |
| X6 | The verdict below; sim-architecture.md amended on this branch to what is now true | **done** |

## 4. The verdict

### Measurements

- **LOC**: net −178 in src/ (447 added, 625 deleted, docs excluded);
  sim/ alone is −143. EntityList.hpp's 350 lines became a 38-line
  Entity.hpp plus ~90 lines of spine ops inside Battle. Tests grew a net
  +36 converting five arena pins into spine/pool pins.
- **Compile time** (per TU, MinGW GCC 15.2, `-g -std=gnu++23`, warm
  second run): Battle.cpp 1.1s → 4.4s, Ship.cpp 0.8s → 2.6s. **EnTT
  costs 3-4× per sim TU.** Tolerable at a dozen TUs; the CI matrix and
  the WASM build will feel it, and it is the experiment's plainest cost.

### The wins ledger, as landed

1. Component machinery became one-liners: attach is `emplace`, lookup is
   `try_get`, teardown is `destroy` — the sidecar vectors, linear finds
   and `dropComponents` are gone, and every M2 component (limpets, charge
   state, marines) is a type, not new infrastructure.
2. `Visual` rides its entity; RenderStore and `purgeDead` deleted. The
   `alive()` guard on attach surfaced a real protocol truth the old
   append-then-purge store papered over (executed overlap spawns).
3. Trait flags became types; the bitfield stopped filling up.
4. `Cloak{level}` ended the every-ship-carries-an-Ilwrath-field
   pollution, and `isCloaked()` *derives* OBJECT_CLOAKED instead of
   storing it — a whole class of desync made unrepresentable.
5. `PriorSilhouette` in Battle.cpp's anonymous namespace: protocol
   scratch nothing else can even name.
6. 350 lines of bespoke arena/handle machinery retired for a library the
   industry tests harder than we ever will.

### The friction ledger, as paid

1. **Compile time, 3-4× per sim TU** (above).
2. **The EntityRef epoch diagnostic is gone** — removed-while-held is
   once again a silent wrong-read in debug builds. EnTT's asserts catch
   less than what we built.
3. **Entity zero is a live entity**: every default-constructed EntityId
   was a landmine; the sweep found one latent uninitialized-enum bug, and
   nothing but discipline prevents the next one.
4. **`in_place_delete` is a per-component judgment that must be right**:
   the hold-across-spawn idiom makes it mandatory for Element and
   ShipState, and nothing in the type system says so — a comment does.
5. **Tags attach to ids, not values**: every spawn site split into
   spawn-then-emplace, and the walk had better not reach the element in
   between (it cannot today; that is an ordering argument, not a check).
6. **Pure functions must be told traits**: applyImpulse takes
   aIsShip/bIsShip because a trait that left the struct cannot be read
   from an Element&.
7. **The ordered walk is still entirely ours**: OrderLink is bespoke code
   the library neither provides nor helps with. It ported correctly on
   the first try *because* the pinned suite existed — without those tests
   this stage would have been genuinely dangerous.

### The declination

The universal core — motion, protocol flags, life/combat numbers,
collision fields, hooks — stays one `Element`. Every element has all of
it; the step protocol reads and writes it as one thing at protocol
points; at forty entities there is no cache argument. The two X5 splits
that DID pay both had an *ownership* story (only the Avenger has a cloak;
only the protocol touches the prior silhouette). The rule the experiment
leaves behind: **split when a component has an owner narrower than
"everything", never for taxonomy.**

### Open questions, answered

1. Version wrap: entt's 32-bit entity is 20-bit index + 12-bit version;
   a melee churns a handful of entities a second spread across slots —
   no realistic path to aliasing, and `alive()` would catch it as a
   wrong-dead, not a wrong-read.
2. The golden battles stayed **bit-green through every stage** — same
   positions, energy, RNG draws, frame counts. Entity id *values* proved
   to be pure bookkeeping, as designed.
3. One registry shared by sim and app worked and read well; the layering
   holds by convention only (sim never names an app type; the app goes
   through Battle's surface for sim state). The convention is written
   down; the compiler does not enforce it. Acceptable for this tree.

### Against sim-architecture.md

Reason 1 (no performance problem) was never contested and remains true —
nothing here was done for speed. Reason 2 (**traversal order is
gameplay**) *held completely*: the order never entered the library, and
the experiment's clearest architectural finding is that a sparse-set
registry coexists gracefully with an owned order spine precisely because
it does not have opinions about order. What the old document rejected —
archetype storage that reorders entities by composition — remains
rejected; flecs would have been that fight. sim-architecture.md is
amended on this branch accordingly.

**Bottom line**: the composition model this tree already believed in got
cheaper — components are types now, and M2's library lands on `emplace`
instead of hand-rolled sidecars — at the price of 3-4× sim compile time,
one lost debug diagnostic, and a handful of disciplines (kNoEntity,
in_place_delete, spawn-then-tag) that comments enforce where the old
types did. Whether that trade merges is SiGMan's call; the branch is the
evidence either way.
