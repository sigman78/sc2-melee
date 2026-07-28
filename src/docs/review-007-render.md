# Review 007 — the dissolution and the app: no flags, no kind, no Element

Two inputs, one program. SiGMan's ask: how far does ECS reach into the
app — melee state as singleton/context, the starfield as an entity with
its stars as data, rendering componentized into ordered passes. And
SiGMan's code review of the branch: Element's flags carried by tags or
component presence; ElementKind implicit in composition; Element split
until it is gone; element hooks dissolved into systems and data; no
transient fields; specs restructured as component payloads; construction
made fluent. The dissolution runs first because the render passes key on
the tags it creates.

The bar for every sim-touching stage: **bit-green, the replay baseline
never moves** — representation, not semantics. Render and app stages are
presentation-only: baseline untouched by construction, screenshots the
proof.

## 1. The target model (the dissolution)

| Was | Becomes |
| --- | --- |
| FiniteLife + lifeSpan (NORMAL_LIFE=1, 0 = died last frame) | `Lifetime{remaining}` — absent means persistent; the C-ism dies. (W3 briefly grew an `ages` bool to preserve the planet's magic lifeSpan=2 — SiGMan's challenge exposed that as the C encoding *indestructibility* through a number; it becomes an `Indestructible` tag on the planet, the bool deletes, and the digest's constant-fold change (2→1, one entity) is the review's first justified re-record, receipted by the **compat-fold protocol** (SiGMan's): temporarily fold the old value via `has<Indestructible> ? 2 : ...`, --compare against the OLD baseline all-32-exact — a bit-exact proof the change moved nothing but the constant — then drop the fold and re-record. This is the standing protocol for any representation change that alters only what the digest folds) |
| Disappearing flag | `Doomed` tag; the reap is a view over it |
| NonSolid flag + mask field + collidable() | `Collider{mask}` — solidity IS having one; runtime toggles are attach/detach |
| Collided, DefyPhysics | Battle-private collision scratch beside PriorSilhouette |
| Appearing, IgnoreSimilar | tags |
| ElementFlags | deleted |
| kind = Weapon/Laser for events and sound | derived from composition at recordSpawn (the event outlives executed spawns, so it must carry what the app needs) |
| kind = IonTrail/ShipShadow/Debris/Blast | Trail/Shadow/Debris/Blast tags (the render passes key on them) |
| ElementKind | deleted |
| Element{current,next,facing} | `Position` |
| Element{velocity} | `Motion` |
| Element{hitPoints} | `Vitality` |
| Element{damage, blastOffset} (+ flame linger) | `Warhead{damage, blastOffset, lingersOnHit}` |
| Element{playerNr, owner} | `Allegiance` |
| Element{colorCycle} | `AnimFrame` |
| Element{turnWait, thrustWait} | ShipState (ship-control clocks; other tenants already left) |
| BeamGeometry tag + current/next abuse | `Beam{from, to}`; beams lack Position, so Integrate/Commit never see them and the exemptions vanish |
| Element | **nothing** |
| onCollision hooks (weapon/flame/solid) | one collision-response system over Warhead/Vitality; the flame is one data bit |
| onDeath hooks (asteroid/rubble/sweep) | `DeathSpawn` payloads + `SweepsOwnedOnDeath` tag |
| preProcess (flame growth) | an animation component + the animate pass |
| ElementHook, collidedWith | deleted — the response system holds both ids as arguments |
| isCloaked() | `Cloaked` tag, maintained solely by the cloak machine at the level transition; a sim_test invariant pins tag ⇔ level==full. (Deliberate reversal of X5's derived-predicate call: the machine is a single pass and the sole writer, and a flag-free world wants composition as truth) |
| WeaponSpec flat mechanic fields | economics (wait, cost, shot basics) + component payloads stamped onto the shot (Guided now; census mechanics later) |
| SpecialSpec | same treatment |
| ShipSpec nesting | ShipDef-level composition: hull/weapon/special as separate spec parts on the definition — the TOML shape |
| Element construction + SpawnCommand verbosity | one fluent spawn builder, designed once the final shape exists, covering immediate and command paths |

Spec-level hooks (ship machines, specials) stay — "fine for now."

**The minimal-composition rule (SiGMan's, from the spawnBeam catch):**
attach a component only where some pass reads it; a pass declares its
component set and is structurally blind to entities lacking it — blanket
get<> before a skip is the god-struct habit wearing a new coat. The PD
beam is the worked example: Beam + Lifetime{1} + Order-until-W7 + Visual,
nothing else. Every W4b+ extraction attaches where read, not uniformly.

**The join rule (SiGMan's, from the eachElement-plus-find travesty):**
Battle grows typed joins — each<Ts...> and eachOrdered<Ts...> yielding
destructured refs — and a pass's component list moves into its call
signature: the read-set as code, not documentation. Per-id find<> inside
an iteration lambda survives only for conditional cross-entity lookups
(another ship's optional Cloak), the legitimately rare case. Extended by
SiGMan mid-W4b into standing policy: **use complex queries proactively** —
presence/absence filters live in the query (view<Ts...> joins,
entt::exclude<Xs...>), only value tests stay in the body
(ageDecrementPass excludes Doomed instead of testing it per iteration;
regen's WarpingIn/Appearing gates likewise). Performance is a non-issue
at this scale; expressiveness is the point — the query IS the pass's
declared meaning. Templates land in W4b's opener; each stage converts
the passes it touches; grep-extinct by W9.

**The X5 reversal, owned:** X5 declined the universal-core split — the
live walk touched everything per-entity, so a split bought ceremony. Z4
changed the ground: the pipeline's passes each read a slice, so
per-concern components make every pass's view declare its true read/write
set. The rule updates: **split when a pass's view would say something
true that the god component hides.**

## 2. Rendering: a declared pipeline of semantic passes

Each pass keyed by the components/tags that identify its content
(SiGMan's call, after two rejected drafts — by layer, then by technique):

    clear → renderStars → renderPlanet → renderAsteroids → renderShips
          → renderProjectiles → renderEffects → marks → hud
          → overlay(ctx-gated) → present

- renderStars: view<Starfield> (the singleton, §3).
- renderPlanet: the Planet tag — owed anyway: gravityPass currently finds
  the well by scanning for mass > 100; the tag cleans sim and render at
  once.
- renderAsteroids: Spin already identifies them.
- renderShips: ShipState — the pass owns the whole ship look (facing
  sprite, cloak tint, warp gating, any future shield glow), so the
  multi-technique-per-entity edge dissolves: hull-then-glow is one pass's
  internal order.
- renderProjectiles: Warhead/Beam between them cover shots and beams.
- renderEffects: the Trail/Shadow/Debris/Blast tags from §1.

Consequences, both simplifications:

1. **CelPolicy retires.** The pass IS the policy; Visual shrinks to pure
   data (sprite set, fallback colour) and visualFor reduces to art
   selection. The enum existed to smuggle per-category draw logic through
   one generic loop; there is no generic loop.
2. **Draw order decouples from sim order.** The C's disp_q conflated
   processing order and z-order in one list, and Layer inherited both
   roles. Now z-order is **(pass, seq)**, declared entirely in the render
   pipeline; Layer remains a purely sim-side ordering concept. Stacking
   choices the old code made by spawn accident (planet over ships) become
   explicit declarations (ships over planet, per the pass order above).

## 3. The app's state joins the world

**True singletons go to entt's context**, not a magic entity: ctx state
has no id, never appears in a view, cannot be reaped. The rule: ctx is
world-scoped state that is no entity's — no id, no lifecycle, one per
world — read by more than one system. The roster:
MatchState{winner, endedAtFrame, shipIds}, Camera (every render pass
reads it; passes then take the world and nothing else),
DebugToggles{overlay, wasDown}, BattleConfig{roster, shipData — same
lifetime as the world whose ShipStates borrow into it}. Battle grows a
narrow typed context surface, setContext<T>/context<T>, mirroring
attach/find (the registry stays private).

Battle's own members (rng, frame, event vectors, command queue) are also
ctx-shaped, and moving them would dissolve Battle into step(registry&) —
declined deliberately: the typed facade IS the registry-privacy boundary,
and every system already receives Battle&. Revisit only if something
wants systems as pure functions of the registry.

**Parallel arrays become components/entities:**

- deathAnnounced[2] → an AnnouncedDead tag on the ship entity: dies with
  the ship, survives M2 fleets where a fixed pair of booleans breaks.
- marks → Mark{event, bornFrame} entities, drawn by their pass, reaped by
  age app-side — entity-fication earns its keep exactly when a pass draws
  them and a TTL reap ages them like everything else.
- the starfield → ONE entity with a Starfield component. Not 180 star
  entities: component granularity follows behavior granularity, and the
  field scrolls as one thing. No Position, no Order — every sim system is
  structurally blind to it.

**What deliberately stays in the app shell:** window, audio, content,
pacer, input accumulators. Device handles and frame plumbing are not
world state; putting them in ctx is a service locator with extra steps.

**Stated invariant:** app-created entities in the shared registry change
entity-id allocation patterns. Harmless — id values feed nothing
gameplay-visible (review-004) and the replay digest does not fold them —
but recorded so nobody later makes ids meaningful without noticing the
app churns them.

## 4. The stages (v2, after the plan critique in §6)

| Stage | What | Proof |
| --- | --- | --- |
| W0 | The construction facade: domain spawn helpers (makeShip/makeAsteroid/makeShot/…) adopted by app, Field, the fire block and the tests — churn armor for every stage after | bit-green; the facade changes no values |
| W1 | Easy flags out: Appearing and IgnoreSimilar to tags; Collided and DefyPhysics to Battle-private collision scratch | bit-green |
| W2 | `Collider{mask}`: solidity is presence, NonSolid dies, collidable() dies | bit-green |
| W3 | `Lifetime{remaining}` + `Doomed`: the aging/death/reap protocol on components; lifeSpan and FiniteLife die; **ElementFlags deleted** (last bits gone). The high-care stage: the died-last-frame protocol and Z4's decrement position are the subtlest pins in the sim | bit-green |
| W4 | The body split, sub-gated one component at a time like Z5: Position → Motion → Beam{from,to} → Vitality → Warhead → Allegiance → AnimFrame → clocks into ShipState; Spawn.hpp's descriptor structs reshaped (they are a mini-Element); **Element deleted** at the end | bit-green per sub-step |
| W5 | Opener: Vec2i grows its operators (+, -, unary -, +=, -=) and the pure vector-math spellings sweep to them (SiGMan: component-wise construction was inherited noise, nothing more). Then hooks dissolve: the collision-response system, DeathSpawn/SweepsOwnedOnDeath, the flame animation component; ElementHook and collidedWith deleted | **done, bit-green throughout** — every hook is data: onCollision dispatches on has\<Warhead\> (weaponCollision/solidCollision take the other id as an argument; the flame's wrapper is `Warhead::lingersOnHit`); onDeath is the `SweepsOwnedOnDeath` tag plus `DeathSpawn{emit}` payloads, run at both death sites; the animate pass iterates Spin (eachOrdered, Z5's proven order) and `FrameDriven` flame growth (plain each\<\> — an empty tag cannot pass through get\<\>); guidedShotPreProcess was vestigial, guidedSteerPass already owned it. Element = {kind}. The spawn-diet finding inverted the plan's hint: Battle::spawn's universal attach set is *correct* — its four callers are all collision-domain — and the diet landed one level up as `spawnEffect` for the never-solid particles, of which IgnoreVelocity died |
| W6 | Planet + effect tags (gravityPass stops scanning by mass); SpawnEvent derives flavor from composition; `Cloaked` tag with its invariant pin, isCloaked deleted. Kind still exists, now unread by sim and sound | bit-green |
| W7 | The semantic render pipeline (§2) on the finished tags; CelPolicy retires, Visual shrinks to data; **ElementKind deleted here** — its last consumer (visualFor's dispatch) dissolves into the passes | baseline untouched by construction; suite 8/8; driven screenshots incl. F1 overlay; stacking changes named in the commit |
| W8 | The app-state migration (§3): Starfield entity, Mark entities + age reap, AnnouncedDead, ctx surface, Battle::destroy for app-owned entities | same gates + screenshots; Game struct visibly shrinks |
| W9 | Specs as payloads; ShipDef-level un-composition; the general fluent builder (final vocabulary exists now); the verdict with measurements (LOC, compile time as observation); sim-architecture.md amended | bit-green; the record |

### Execution shift after W5 (SiGMan, 2026-07-28)

The remaining stages run batched, not sequential — the per-stage cadence
proved too slow and token-expensive. The shape: the main session pre-lands
a **skeleton commit** carrying every shared surface (the W6 tags, Battle's
setContext/context/destroy, the builder shell) so the remaining work
partitions into two disjoint-file tracks that cannot conflict by
construction; then **W6+W9 (sim track) and W7+W8 (app track) execute as
two parallel agents in separate git worktrees** with their own build dirs.
The one entangled deletion — ElementKind, which the app track stops
reading while the sim track still writes — is deferred to a small
post-merge harmonization sweep. Gates are unchanged in kind, batched in
frequency: each track runs its full battery at its two stage boundaries;
the merged result gets one final battery plus the driven visual check.
Track briefs staged in `.claude/briefs/`.

## 5. Risks named

- The replay digest reads Element fields; each split re-points it at the
  new components. Same values, same order, same hashes — any digest
  change is a bug in the split, not a legal divergence.
- sim_test touches Element everywhere; the sweeps are large but
  mechanical. Expectations never change; only field access does.
- W5 is the judgment stage (behavior → data); adjudicated checkpoint
  like Z4's if it grows teeth.

## 6. The plan critique (why v2 differs from v1)

- **Ordering bug found:** v1 deleted ElementKind one stage before the
  render passes replaced its last consumer — visualFor still dispatches
  on kind until the semantic passes exist. Deletion moved into the
  render stage as its closing act.
- **Reversal:** the fluent builder was scheduled last ("design once the
  shape is final"); wrong optimization. A domain-level facade FIRST
  absorbs the construction churn of every dissolution stage — five
  sweeps of a hundred sites become edits inside a handful of helpers.
  The component-level fluent builder still lands last; facade is churn
  armor, builder is polish, and they are different things.
- **Risk ordering:** v1 opened with Lifetime — the subtlest protocol in
  the sim (died-last-frame, Z4's decrement position). v2 runs easy to
  hard: trivial tags, then Collider, then Lifetime with due care.
- **Omissions repaired:** Spawn.hpp's descriptor structs (a
  mini-Element) added to the body split; the verdict got its
  measurement list. (entt PCH was briefly added at W0, then dropped on
  SiGMan's call — not a big deal at current scale; it stays a known
  lever in review-004's ledger.)
- **Challenged and kept:** marks as entities (taxonomy-ish, justified
  by pass + TTL uniformity); Appearing as a tag (churn on newborns
  only; nowhere better once the bitfield dies); body-split-before-hooks
  (dissolving hooks first would avoid double-sweeping them, but the
  response system's clean expression needs Warhead/Vitality to exist).

## 7. Sim-track verdict (W6 + W9) — DRAFT, main session to edit

Executed in the w-sim worktree, parallel to the app track (W7+W8), off the
skeleton commit. Both gates green throughout; no baseline re-record.

**W6.** `Planet` attached at `spawnPlanet`; `gravityPass` now finds the well
via `each<Planet>` instead of scanning every collidable's mass — the scan
survives only inside `calculateGravity` itself (that function still answers
"is X inside somebody else's well" for an arbitrary `X`, ship or planet
alike, so it keeps testing mass there). `Trail`/`Shadow`/`Debris`/`Blast`
attach through four new `SpawnCommand` bools (`trail`/`shadow`/`debris`/
`blast`), set at each effect's own spawn site and consumed in
`drainSpawnCommands`. `Cloaked` is maintained solely by `ilwrathPreProcess`
(one sync point at the end of the function, checked against every level
change made earlier in the same call); `isCloaked()` is deleted, its three
callers (Targeting.cpp, Human.cpp, ShipSystems.cpp) read `has<Cloaked>`
directly. `ElementKind` survivors in src/sim: zero reads outside the
`Element::kind` field declaration and the unchanged spawn-site writes —
`recordSpawn` no longer reads it at all, deriving `SpawnEvent::kind` from
`has<Beam>`/`has<Warhead>` composition instead (§1's "derived from
composition" row, made literal). `Warhead` moved into `Battle::spawn`'s own
parameter list (attached before `recordSpawn`, mirroring `collider`) so
that derivation is a real registry read, not a guess ahead of one.

**W9.** `WeaponSpec` gained `Lifetime`/`Vitality`/`Warhead` fields and an
`std::optional<Guided>` in place of the six scalars (`life`, `damage`,
`hitPoints`, `blastOffset`, `trackWait`, `maxSpeed`, `thrustScale`) that
used to be manually repacked into components at every shot; `speed` and
`muzzleOffset` stayed scalar (geometry, resolved against the ship's own
facing). The `Guided` case is the clean one — `cmd.guided = spec.weapon.guided;`,
verbatim, no presence heuristic. The `Warhead`/`Vitality`/`Lifetime` cases
are a partial win: the *spec's own storage* is now attach-shaped, but the
fire block still reads `sp.damage`/`sp.hitPoints`/`sp.life` off the per-shot
`Spawn` descriptor (Spawn.hpp), not off the spec directly, because those
three are legitimately per-shot-overridable in the AI-lookahead design
(Spawn.hpp's own stated purpose) and Spawn.hpp was out of this stage's
scope — `earthlingCruiser()`/`ilwrathAvenger()` read as flat literals either
way, since none of the two ships ever exercises that override today.
`SpecialSpec` needed no change: nothing there was ever translated into a
component (`cruiserSpecial`'s beam/damage are computed per-target, not
spec-constant).

`Battle::spawn`/`spawnBeam`/`spawnEffect` (both overloads) now build through
`make()` internally and return `Spawned` instead of `EntityId` — a strictly
backward-compatible signature change (`Spawned` converts implicitly), so
every existing `const EntityId id = b.spawn(...)` callsite still compiles,
while `spawnPlayerShip`, `spawnPlanet`, `spawnAsteroid` and
`drainSpawnCommands` were rewritten to chain their extra attaches through
`.with(...)` on the result instead of a further `Battle::attach<T>(id, ...)`
call. `attachWeaponSpec` (setter half) is deleted — its one caller now does
`.with(WeaponGuidance{spec})` directly; the getter (`weaponSpec()`) stays,
still used by `animatePass` and `guidedShotPreProcess`. One gap found and
fixed in the shell: `Spawned::with(T&&)` had no path for an empty (tag)
component — entt's own `emplace` for a zero-size type takes no value
argument, so `.with(Appearing{})`-style calls for `PlayerShip`,
`WarpingIn`, `FrameDriven`, `Trail`, etc. did not compile until `with`
learned to branch on `std::is_empty_v<T>`.

Join-rule extinction grep (every `each<Ts...>`/`eachOrdered<Ts...>` with a
non-empty `Ts` across src/sim): one survivor, `Battle::animatePass`'s
`each<AnimFrame, FrameDriven>` lambda, `find<Collider>(id)`. Same-entity, not
cross-entity — the flame's post-detonation linger frame legitimately has no
Collider (already detached by its own death), but must keep advancing
`AnimFrame` regardless, so folding `Collider` into the join's required set
would silently stop that advance instead of just skipping the mask update.
Already documented in place before this stage; left as is.

**Measurements** (collected, not editorialized):

| Measurement | Value |
| --- | --- |
| `git diff --stat` vs merge-base(main) for src/sim + sim_test.cpp | 33 files changed, 8963 insertions(+) — main predates the ECS rewrite entirely, so this is the whole subsystem, not this stage |
| `git diff --stat` vs the pre-W6 skeleton commit (this stage's own delta) | 11 files changed, 272 insertions(+), 201 deletions(-) |
| Cold build wall-clock (`cmake --build build/core --target clean` then `cmake --build build/core`) | 45.8s real |
| `replay_test --compare` | all 32 battles matched exactly (both stage boundaries) |
| `ctest` | 10/10 passed (both stage boundaries) |

**Component census** (component → attach set), sim components only:

| Component | Attached at |
| --- | --- |
| Element | every sim spawn (`spawn`/`spawnBeam`/`spawnEffect`) |
| Position | `spawn`, `spawnEffect` (both overloads) |
| Motion | `spawn`, `spawnEffect`'s drifting overload (debris) |
| Physique | `spawn` only |
| Allegiance | `spawn`, `spawnBeam`, `spawnEffect` — the one uniform attach |
| PriorSilhouette (Battle-private) | `spawn` only |
| CollisionScratch | `spawn` only |
| Order | `make()`, i.e. every sim spawn transitively |
| Appearing | `spawn` only |
| Collider | `spawn` (iff a mask given); reattached by `applyFacingMask`, warp arrival |
| Beam | `spawnBeam` only |
| IgnoreSimilar | `spawnPlayerShip` (always); the Ilwrath flame shot (`sp.ignoreSimilar`) |
| Lifetime | queued shots/effects (`cmd.lifetime`); ship warp-in and explosion clocks |
| Doomed | `ageAndReapMarkPass`, `killOverlapSpawn`, `weaponCollision`, `sweepDeadShipOrdnance` |
| Indestructible | `spawnPlanet` only |
| Planet | `spawnPlanet` only (W6) |
| Trail | `spawnIonTrail` via `cmd.trail` (W6) |
| Shadow | `warpInStep` via `cmd.shadow` (W6) |
| Debris | `explosionStep` via `cmd.debris` (W6) |
| Blast | `asteroidDeath`, `weaponCollision`, via `cmd.blast` (W6) |
| Cloaked | `ilwrathPreProcess` only (W6) |
| PlayerShip | `spawnPlayerShip` only |
| Vitality | `spawnPlanet`, `spawnAsteroid`, weapon shots (`cmd.vitality`) |
| AnimFrame | weapon shots (`cmd.animFrame`) |
| FrameDriven | the Ilwrath flame shot only (`spec.weapon.frameDriven`) |
| Guided | guided shots only (the Cruiser nuke) |
| ShipState | `attachShip` (via `spawnPlayerShip`) |
| Input | `attachShip` (bundled with ShipState) |
| WeaponGuidance | weapon shots, `.with(WeaponGuidance{...})` in `drainSpawnCommands` |
| Cloak | `ilwrathPreProcess`, lazily on first use |
| WarpingIn | `spawnPlayerShip` (iff warping in); detached on arrival |
| Exploding | `startShipExplosion`; detached after the burn |
| SweepsOwnedOnDeath | `startShipExplosion` |
| StashedMask | `spawnAsteroid`, warp-in's Appearing branch, `asteroidDeath`'s rubble |
| DamageIncoming | `doDamage` (ship branch); cleared every frame |
| Warhead | `spawn`, iff a weapon (`cmd.warhead` forwarded in) |
| Spin | `spawnAsteroid` only |
| DeathSpawn | `spawnAsteroid`, `asteroidDeath`'s rubble (`rubbleDeath` payload) |
