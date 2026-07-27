# Review 002 — the data-driven composition move

The assessment SiGMan asked for: benefits and feasibility of taking the sim
to a data-driven, ECS-composed shape — ships declared as data, weapons and
specials generalized into a component library, cross-cutting behavior
extracted into systems. Verdict up front: **feasible now, beneficial now,
and most of the groundwork is already laid** — the spec extraction, hook
slots and the ordered-entity spine were built for exactly this. What follows
is the evidence, the design, and the staged plan. docs/sim-architecture.md
holds the standing commitments this review executes against (composition
yes, archetype storage no, order is gameplay).

## 1. The Element problem, dissected

`Element` today is a fat struct with an `ElementKind` enum, and the smell
SiGMan named is real — but the enum plays three different roles and only one
of them is rotten:

1. **Behavior dispatch in the sim** — exactly two `kind == Laser` checks in
   the step loop (skip the Appearing seed, skip the commit). A beam's
   `current`/`next` are geometry, not motion; that is a property of the
   *element*, not knowledge the *loop* should hold. → becomes the
   `BeamGeometry` trait flag, set at spawn. Deletes all sim-side kind
   dispatch.
2. **Presentation dispatch in the app** — `spritesFor`/`colourFor` switches
   deduce what to draw from sim identity. → becomes render components the
   app attaches in its own `EntityId`-keyed store, driven by the
   `SpawnEvent` channel that already reports every spawn (including the ones
   sim hooks make). Cel policy — by-facing, by-animation, ramp-tint,
   beam-line — becomes attach-time data; `colorCycle`'s double life as sim
   state and cel index ends.
3. **Tagging** — `SpawnEvent.kind`, sounds, tests, debugging. Legitimate and
   kept: this is what D21 (a real tag instead of the C's
   frame-pointer-as-identity) was actually for. It never needed to be a
   dispatch mechanism to do that job.

Why not the type/topological branch (variant or inheritance over per-kind
element types): the C's one deep insight here is that an element's behavior
is its **hooks and flags, not its class** — that genericity is what let 25
ships invent turrets, limpets, marines, phoenixes and gas clouds without
touching the core. A closed sum type re-centralizes every future mechanic
into one variant, adds visitor dispatch to a loop that is genuinely uniform
over motion/flags/collision, and does not even slim the struct (a variant is
as large as its largest member).

The fat-struct half of the problem is state, and it splits along kind lines:
`ShipState` (~60 bytes) rides on every debris spark today, and weapons
smuggle their spec pointer through `ShipState::spec` because there is
nowhere else to put it. Both are components in the wrong place:

- **ShipState** → sidecar store keyed by `EntityId`, owned by `Battle`.
- **Weapon guidance** → its own small component (`Borrowed<const
  WeaponSpec>` plus nothing, today), ending the "a weapon is not a ship but
  carries a ship's state" abuse in `shipPostProcess`.

Storage is a flat `std::vector<std::pair<EntityId, T>>` with linear find —
a melee holds a handful of ships and shots, rule 1 says no cleverness before
a profile, and the stable `EntityId` keying keeps the ordered-list spine
untouched (order stays gameplay).

## 2. Systems already latent in the two shipped ships

`shipPreProcess`/`shipPostProcess` plus the hooks already contain the
system/component inventory in embryo. Naming them is most of the work of
extracting them:

| Latent unit | Where it hides | Component or system? |
| --- | --- | --- |
| Energy clock (regen + counter re-arm) | shipPreProcess + deltaEnergy | system over ships |
| Turning | shipPreProcess | system over ships |
| Thrust + exhaust | shipPreProcess → thrust() | system over ships (thrust() is already a pure primitive) |
| Weapon fire (gate, pay, spawn) | shipPostProcess | system over ships, parameterized by WeaponSpec |
| Special gate (counter, key) | shipPostProcess | system over ships, dispatching SpecialSpec.hook |
| Guided flight | nukePreProcess | component: GuidedShot(trackWait, maxSpeed, thrustScale) |
| Growing animated shot | flamePreProcess/flameCollision | component: AnimatedShot(+linger-on-hit) |
| Point defence | cruiserSpecial | component: PointDefence(range, cost) |
| Cloak machine | ilwrathPreProcess | component: Cloak(colours, cost, wait) + ambush aim |
| Warp-in | shipTransition | system (every ship arrives the same way) |
| Explosion/debris | startShipExplosion/explosionPreProcess | system |
| Ion trail | spawnIonTrail | system |
| Gravity | Gravity.cpp | system (already extracted) |
| Asteroid field | Field.cpp | system (already extracted) |

The pattern: **systems are the phases every ship shares; components are the
parameterized behaviors a spec opts into.** The hook slots stay the dispatch
(sim-architecture.md's commitment — every C ship was tuned against direct
per-entity dispatch); what changes is that slot *values* come from a named
library configured by spec data instead of being ad-hoc free functions.

## 3. Ships as data — what is missing after the spec extraction

`ShipSpec` is one step from `ShipSpec{.maxCrew = 18, .thrust{.max = 24,
.increment = 3}, .weapon{...}, .special{...}}` as a designated-initializer
literal. The remaining gap is that hooks are raw function pointers rather
than named library entries with parameters. The cloak's colour count and
PD's range already live in the spec; the rule generalizes: **a component's
tuning lives in the spec, its code lives in the library, and a ship file
(in code today, TOML later — same shape) is stats + a list of component
choices.** No registry machinery yet: a component "entry" is a function
pointer plus its parameter block in the spec, and that stays true until the
modding question (plan open question 1) forces a registry.

## 4. The census — reuse measured across all 28 ship files

Produced by a full read of `sc2/src/uqm/ships/` (25 races plus probe,
lastbat, sis_ship). Two engine-level idioms recur across unrelated ships
and are infrastructure gaps, not gameplay components:

- **Self-chaining elements** — a weapon/effect clones itself into a fresh
  element every frame (blackurq buzzsaw, orz turret, slylandro lightning,
  the three point-defence triggers, zoqfot tongue). Wants a first-class
  helper, not six hand-rolled copies.
- **Field repurposing** — `special_counter`/`special_wait`/`turn_wait`
  hijacked as ad-hoc scratch (chenjesu's drone-alive flag, blackurq's saw
  count, urquan's fighter count, chmmr's laser colour index, orz's marine
  gravity nibbles). This is the strongest direct evidence for E2: typed
  per-entity component state is exactly what these hacks are missing.

### Reuse matrix (components with ≥2 users)

| Component | Ships |
| --- | --- |
| StraightShot | druuge, ilwrath, shofixti, spathi, supox, syreen, thradd, urquan, yehat, zoqfot, sis_ship |
| GuidedShot(trackWait, maxSpeed, thrustScale) | androsyn, human, mmrnmhrm(Y), mycon, blackurq(residue), spathi(special), sis_ship |
| Beam | arilou(auto-aim), chmmr, vux, mmrnmhrm(X) |
| PointDefenceTurret (self-respawning scan-and-fire) | human, chmmr(satellites), sis_ship |
| ChargeWeapon(whileHeld, onRelease) | chenjesu, melnorme, blackurq(partial) |
| SprayShot(n, arc) | pkunk(3), utwig(6 paired), yehat(2), blackurq(16, special), sis_ship |
| Melee hitbox | umgah(primary), zoqfot(tongue) |
| ShieldOnSpecial | utwig(absorb→energy), yehat(invulnerability) |
| ChildSpawner | orz(marines), chenjesu(drone), urquan(fighters), chmmr(satellites) |
| Drain/Leech (any stat) | chenjesu(energy), slylandro(environment), syreen(crew) |
| Regeneration | mycon(crew), pkunk(taunt self-energy) |
| AoEField (lingering hazard) | thradd(napalm trail), blackurq(gas clouds) |
| OmniThrust/RetroThrust | supox(facing-override), thradd(afterburner), umgah(impulse) |
| TransformStance (alt-configuration swap) | androsyn(forced revert), mmrnmhrm(persistent toggle) |
| KnockbackOnHit | druuge, lastbat(sentinel) |
| StatModifier (limpet) | vux(live), shofixti(same formula, static) |

Single-user but clean components: Cloak (ilwrath, shipped), Teleport
(arilou), DeathPayload (shofixti), TurretMountedShot (orz), ChainBeam
(slylandro), melee-cone facing cache aside.

### The true one-offs (irreducible, ~8 of 28)

| Ship | Irreducible mechanism |
| --- | --- |
| probe | no combat behavior at all — inert encounter shell |
| lastbat | the Sa-Matra boss state machine (generators, gate, comets, sentinels) |
| sis_ship | runtime RACE_DESC assembly from player loadout + HQ/battle hook swap — a ship *builder* |
| pkunk | phoenix respawn: the saved/restored hook triple across death |
| melnorme | confusion overwrites the victim's input state — the only input hijack |
| chmmr | tractor writes the target's velocity/status every frame — the only continuous motion hijack |
| vux | aggressive entry: one-shot self-deleting spawn-near-enemy preprocess |
| orz | the turret-aim wrapper rewriting the ship's own FIRES_* flags (the marines themselves reuse ChildSpawner) |

The plan's "~15 components, ~19 one-offs" refines to **16 multi-user
families and ~8 irreducible mechanisms** — composition wins more of the
roster than the original estimate credited.

### First three M2 ships, chosen to force generality

1. **Human** — GuidedShot (the accelerating nuke is the literal source of
   the thrustScale parameter set) + PointDefenceTurret, zero one-off code.
   (Already shipped; the component extraction retrofits it.)
2. **Utwig** — SprayShot(paired offsets) + ShieldOnSpecial's richer
   damage-to-energy variant, zero one-off code.
3. **Urquan** — StraightShot + ChildSpawner(fighters) with the
   boomerang-return-for-crew behavior, zero one-off code.

Together: six of the multi-user families exercised before any boss-fight,
hook-swap or cross-file special case is touched.

## 5. The dry runs — Supox, Chmmr, Arilou as pure spec + components

The second-user rule applied in advance: all three ships were written out
as full `ShipSpec` literals against today's types, every number cited to
its `#define`. Verdicts:

- **Supox: expressible today, minus tactics.** OmniThrust is *simpler*
  than the plan's worked example promised — `thrust()` already takes
  `Facing` explicitly, so the C's save/overwrite/restore dance is
  structurally impossible to need. The plan's worked example holds up with
  two corrections: its `speed = 30` is the pre-`DISPLAY_TO_WORLD` literal
  (the spec field needs 120 — a per-field conversion a loader must know),
  and its `sphereRadius = 333` is the C's numerator, not the stored
  strength field (333/11·2 = 60). The irreducible part is
  `supox_intelligence`'s ~60 lines of tactics, per plan.
- **Arilou: expressible modulo the beam shape.** The teleport is a clean
  preprocess-phase component (2 RNG draws, uniform arena placement with
  NO overlap rejection, 5-frame NonSolid window, relocation on frame 3,
  cooldown clocks frozen by out-incrementing the engine's decrements,
  input replayed from a snapshot). Its auto-aim needs *no new primitive*
  — our `trackShip` already returns the delta and target; the caller
  snaps instead of nudging. Deliberately lacks FIRES_FORE, which is
  load-bearing for the AI layer.
- **Chmmr: the model's honest boundary, as predicted.** Satellites are
  physically solid but INVINCIBLE (`satellite_collision` is an empty
  stub; SATELLITE_HITPOINTS is dead data), orbit by repurposing
  `turn_wait` as the angle, station-keep with snap-within-20px else
  capped pursuit, and die only via the dead-ship ordnance sweep. The
  tractor pulls the target toward a point 272 world units in FRONT of
  Chmmr (LASER_RANGE/3 + offset), impulse 12/targetMass, then hand-patches
  the target's speed flags — engine primitive #2's own poster case. The
  chmmr.c:773 self-nulling preprocess hook is unrepresentable against an
  immutable shared spec, exactly as `ShipSpec`'s docs predicted.

### The ranked gap list (what the sim lacks, by how many ships need it)

1. **Instant beam/line weapon shape** (`WeaponSpec::kind = Beam`, range) —
   blocks Chmmr's and Arilou's primaries; today's only precedent is
   `cruiserSpecial`'s hand-coded beam.
2. **Segment-vs-mask collision** — sweptIntersect is mask-vs-mask only;
   every laser in the game needs the line test.
3. **AI/tactics layer** (`EVALUATE_DESC`/`ship_intelligence`/`MoveState` +
   `ShipSpec::aiHints`) — blocks all three `*_intelligence` functions;
   scheduled as M2's cyborg.c port.
4. **`ShipSpec::shipFlags`** (FIRES_FORE, IMMEDIATE_WEAPON,
   SEEKING_SPECIAL, POINT_DEFENSE...) — needed by all three; Arilou's
   *lack* of FIRES_FORE is load-bearing; POINT_DEFENSE must be *derived*
   from live satellites, not stored (the plan's accidental-refcount case).
5. **Unconditional post-phase ship hook** (`ShipSpec::postProcess`) —
   Chmmr's muzzle flash is WEAPON-driven, not SPECIAL-driven.
6. **One-time-per-instance setup slot** — the chmmr.c:773 self-nulling
   hook becomes an `onArrival`-style slot or a component with a done-bit;
   mutating the shared spec stays impossible by design.
7. **Per-ship scratch state beside the SPECIAL cooldown** — Chmmr rides
   its laser colour phase on `special_counter`, which our engine owns;
   sidecar components (E2) are the answer.
8. **Post-fire handle** — `spawnBack` already returns the id; the generic
   fire block discards it, forcing the C's GetTailElement workaround.
9. **Counter-freeze + previous-input snapshot** — Arilou's mid-teleport
   state; declare it rather than hand-cancel the decrements.
10. **DISPLAY_ALIGN helper in World.hpp** — one line, used by teleport
    placement (and already inlined twice in Field.cpp).
11. **Muzzle-from-art-rect** — Chmmr only; polar offset covers the rest.
12. **Throttled per-element animation clock** (count down, advance,
    reset) — chmmr's `animate` reused by two element kinds; our
    `colorCycle` is a bare increment.

Also confirmed as a spec-authoring rule: **`WeaponSpec::speed` stores the
post-`DISPLAY_TO_WORLD` value; offsets store raw display pixels** — the
convention today's two ships already follow, now written down because the
Supox worked example tripped on it.

## 6. Costs, called against the two constraints

- **cyborg.c-first (M2 strategy).** The AI reads ships through
  `ship_intelligence`/intercept lookahead, not through our hook internals —
  the component move does not shift the reference frame the AI port needs,
  PROVIDED the phase timing pinned by review-001 stays fixed. Every stage
  below is behavior-preserving under the existing test suite, which is the
  proof mechanism.
- **Faithfulness invariants.** Dispatch order, hook phases, RNG draw order
  are all pinned by tests now. Component extraction moves code between
  files; the tests do not care where the code lives, only when it runs.
- **Real cost:** the sidecar access pattern (`b.ship(id)` instead of
  `e->ship`) is a whole-tree sweep and a small ergonomic tax at every ship
  site. Paid once, mechanically, by subagent.

## 7. The staged plan

| Stage | What | Risk |
| --- | --- | --- |
| E1 | `BeamGeometry` trait flag; sim-side kind dispatch → zero | trivial |
| E2 | `ShipState` + `WeaponGuidance` sidecar components on `Battle`; `Element` slims to the universal core + tag | mechanical sweep, tests as referee |
| E3 | App-side render components attached on SpawnEvents; draw loop iterates the store; `spritesFor` switch retired | app-only |
| E4 | Spec literals: the two ships rewritten as designated-initializer `ShipSpec` values over named library components | cosmetic |
| E5 (M2, per ship) | Each new mechanic lands as a component; systems formalized as the phase functions they already are | per plan |

Stages E1–E4 are this review's execution; E5 is M2 absorbing the model.

## 8. Out-takes

- The ordered `EntityList` never changes: composition rides on stable ids,
  and the storage question stays closed (sim-architecture.md, reopened only
  by a profile).
- `ElementFlags` is close to full (bit 12 with BeamGeometry). Widening to
  64-bit or splitting sim-flags/trait-flags is a decision to take when bit
  15 appears, not before.
- `Turret` sits unused in `ElementKind` — left until the census says what
  turrets actually need.
- The C's per-instance hook *mutation* (chmmr.c:773, pkunk.c:282) remains
  the acknowledged edge: components with internal state machines replace
  self-modifying dispatch, per the plan's §What we accept losing.
