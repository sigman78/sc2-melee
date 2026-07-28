# Review 008 — the dissolution: no flags, no kind, no Element

SiGMan's second opinion after reading the branch, turned into the closing
program: Element's remaining flags carried by tags or component presence;
ElementKind implicit in composition; Element itself split until it is
gone; element hooks dissolved into systems and data; no transient fields;
specs restructured as component payloads; construction made fluent. The
whole review is representation, not semantics: **bit-green throughout, the
replay baseline never moves.**

## 1. The target model

| Was | Becomes |
| --- | --- |
| FiniteLife + lifeSpan (NORMAL_LIFE=1, 0 = died last frame) | `Lifetime{remaining}` — absent means persistent; the C-ism dies |
| Disappearing flag | `Doomed` tag; the reap is a view over it |
| NonSolid flag + mask field + collidable() | `Collider{mask}` — solidity IS having one; runtime toggles are attach/detach |
| Collided, DefyPhysics | Battle-private collision scratch beside PriorSilhouette |
| Appearing, IgnoreSimilar | tags |
| ElementFlags | deleted |
| kind = Weapon/Laser for events and sound | derived from composition at recordSpawn (the event outlives executed spawns, so it must carry what the app needs) |
| kind = IonTrail/ShipShadow/Debris/Blast | Trail/Shadow/Debris/Blast tags (the render passes want them anyway) |
| ElementKind | deleted |
| Element{current,next,facing} | `Position` |
| Element{velocity} | `Motion` |
| Element{hitPoints} | `Vitality` |
| Element{damage, blastOffset} (+ flame linger) | `Warhead{damage, blastOffset, lingersOnHit}` |
| Element{playerNr, owner} | `Allegiance` |
| Element{colorCycle} | `AnimFrame` |
| Element{turnWait, thrustWait} | ShipState (they are ship-control clocks; other tenants already left) |
| BeamGeometry tag + current/next abuse | `Beam{from, to}`; beams lack Position, so Integrate/Commit never see them and the exemptions vanish |
| Element | **nothing** |
| onCollision hooks (weapon/flame/solid) | one collision-response system over Warhead/Vitality; the flame is one data bit |
| onDeath hooks (asteroid/rubble/sweep) | `DeathSpawn` payloads + `SweepsOwnedOnDeath` tag |
| preProcess (flame growth) | an animation component + the animate pass |
| ElementHook, collidedWith | deleted — the response system holds both ids as arguments |
| isCloaked() | `Cloaked` tag, maintained solely by the cloak machine at the level transition; a sim_test invariant pins tag ⇔ level==full. (Deliberate reversal of X5's derived-predicate call: the machine is now a single pass and the sole writer, and a flag-free world wants composition as truth) |
| WeaponSpec flat mechanic fields | economics (wait, cost, shot basics) + component payloads stamped onto the shot (Guided now; census mechanics later) |
| SpecialSpec | same treatment |
| ShipSpec nesting | ShipDef-level composition: hull/weapon/special as separate spec parts on the definition — the TOML shape |
| Element construction + SpawnCommand verbosity | one fluent spawn builder, designed once the final shape exists, covering immediate and command paths |

Spec-level hooks (ship machines, specials) stay — "fine for now."

## 2. The X5 reversal, owned

X5 declined the universal-core split: the live walk touched everything
per-entity, so a split bought ceremony. Z4 changed the ground — the
pipeline's passes each read a slice, so per-concern components make every
pass's view declare its true read/write set. The rule updates: **split
when a pass's view would say something true that the god component hides.**

## 3. The stages (each bit-gated; re-record never)

| Stage | What |
| --- | --- |
| V1 | `Lifetime` + `Doomed`: the aging/death/reap protocol on components; lifeSpan and FiniteLife die |
| V2 | `Collider` + private collision scratch + Appearing/IgnoreSimilar tags; ElementFlags deleted |
| V3 | The body split: Position/Motion/Vitality/Warhead/Allegiance/AnimFrame; Beam{from,to}; turnWait/thrustWait into ShipState; Element deleted; the fluent spawn builder lands here (construction shape is final) |
| V4 | Hooks dissolve: collision-response system, DeathSpawn/SweepsOwnedOnDeath, flame animation component; ElementHook and collidedWith deleted |
| V5 | ElementKind deleted: effect tags land; SpawnEvent derives flavor from composition; `Cloaked` tag with its invariant pin, isCloaked deleted |
| V6 | Specs as payloads; ShipDef-level un-composition; the verdict |

Sequencing against review-007: **008 first, then 007** — the render
passes key on tags this review creates; running 007 before it would
build passes on kind-filters scheduled for deletion.

## 4. Risks named

- The replay digest reads Element fields; each split re-points it at the
  new components. Same values, same order, same hashes — any digest
  change is a bug in the split, not a legal divergence.
- sim_test touches Element everywhere; the sweeps are large but
  mechanical. Expectations never change; only field access does.
- V4 is the judgment stage (behavior → data); its commit must name every
  hook dissolved and what carries its behavior now.
