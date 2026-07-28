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
| FiniteLife + lifeSpan (NORMAL_LIFE=1, 0 = died last frame) | `Lifetime{remaining}` — absent means persistent; the C-ism dies |
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
has no id, never appears in a view, cannot be reaped. Candidates:
MatchState{winner, endedAtFrame}, the debug-overlay toggle, the
roster/shipData pair. Battle grows a narrow typed context surface (the
registry stays private).

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

## 4. The stages

| Stage | What | Proof |
| --- | --- | --- |
| W1 | `Lifetime` + `Doomed`: the aging/death/reap protocol on components; lifeSpan and FiniteLife die | bit-green |
| W2 | `Collider` + private collision scratch + Appearing/IgnoreSimilar tags; ElementFlags deleted | bit-green |
| W3 | The body split: Position/Motion/Vitality/Warhead/Allegiance/AnimFrame; Beam{from,to}; turnWait/thrustWait into ShipState; Element deleted; the fluent spawn builder lands here | bit-green |
| W4 | Hooks dissolve: collision-response system, DeathSpawn/SweepsOwnedOnDeath, flame animation component; ElementHook and collidedWith deleted | bit-green; the commit names every hook dissolved and what carries its behavior |
| W5 | ElementKind deleted: Planet + effect tags land (gravityPass stops scanning by mass); SpawnEvent derives flavor from composition; `Cloaked` tag with its invariant pin, isCloaked deleted | bit-green |
| W6 | The semantic render pipeline (§2) on the finished tags; CelPolicy retires, Visual shrinks to data | baseline untouched by construction; suite 8/8; driven screenshots incl. F1 overlay; deliberate stacking changes named in the commit |
| W7 | The app-state migration (§3): Starfield entity, Mark entities + age reap, AnnouncedDead, ctx surface, Battle::destroy for app-owned entities | same gates + screenshots; Game struct visibly shrinks |
| W8 | Specs as payloads; ShipDef-level un-composition; the verdict; sim-architecture.md amended | bit-green; the record |

## 5. Risks named

- The replay digest reads Element fields; each split re-points it at the
  new components. Same values, same order, same hashes — any digest
  change is a bug in the split, not a legal divergence.
- sim_test touches Element everywhere; the sweeps are large but
  mechanical. Expectations never change; only field access does.
- W4 is the judgment stage (behavior → data); adjudicated checkpoint
  like Z4's if it grows teeth.
