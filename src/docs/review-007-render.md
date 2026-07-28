# Review 007 — the app joins the world: render passes, context, singletons

SiGMan's ask: how far does ECS reach into the app? Melee state as
singleton/context, the starfield as an entity with its stars as data, and
rendering componentized into ordered layered passes. Answer: the expensive
prerequisites were reviews 004-006 (shared registry, Visual as component,
Order{layer, seq}, eachOrdered); what remains is presentation-only,
sim-untouched, and therefore cheap to verify — the replay baseline cannot
move, the suite's expectations do not change, and screenshots are the
proof of behavior.

## 1. The design

**Rendering becomes a declared pipeline of semantic passes**, each keyed
by the components/tags that identify its content (SiGMan's final call,
after two rejected drafts — by layer, then by technique):

    clear → renderStars → renderPlanet → renderAsteroids → renderShips
          → renderProjectiles → renderEffects → marks → hud
          → overlay(ctx-gated) → present

- renderStars: view<Starfield> (the W2 singleton).
- renderPlanet: a Planet tag — owed anyway: gravityPass currently finds
  the well by scanning for mass > 100; the tag cleans sim and render at
  once.
- renderAsteroids: Spin already identifies them; the tag exists in all
  but name.
- renderShips: ShipState/PlayerShip — the pass owns the whole ship look
  (facing sprite, cloak tint, warp gating, any future shield glow), so
  the multi-technique-per-entity edge dissolves: hull-then-glow is one
  pass's internal order.
- renderProjectiles: weapons and beams (Guided/BeamGeometry/
  WeaponGuidance cover them).
- renderEffects: trails, shadows, debris, blasts — kind-filtered until a
  tag earns its keep.

Consequences, both simplifications:

1. **CelPolicy retires.** The pass IS the policy; Visual shrinks to pure
   data (sprite set, fallback colour) and visualFor reduces to art
   selection. The enum existed to smuggle per-category draw logic
   through one generic loop; there is no generic loop.
2. **Draw order decouples from sim order.** The C's disp_q conflated
   processing order and z-order in one list, and Layer inherited both
   roles. Now z-order is **(pass, seq)**, declared entirely in the
   render pipeline; Layer remains a purely sim-side ordering concept.
   Two orders, two declarations, no coupling — and stacking choices the
   old code made by spawn accident (planet over ships) become explicit
   declarations (ships over planet, per the pass order above).

**True singletons go to entt's context**, not a magic entity: ctx state
has no id, never appears in a view, cannot be reaped. Candidates:
MatchState{winner, endedAtFrame}, the debug-overlay toggle, the
roster/shipData pair. Battle grows a narrow typed context surface
(the registry stays private).

**World state that was parallel arrays becomes components/entities:**

- deathAnnounced[2] → an AnnouncedDead tag on the ship entity: dies with
  the ship, survives M2 fleets where a fixed pair of booleans breaks.
- marks → Mark{event, bornFrame} entities, drawn by their pass, reaped by
  age app-side. Entity-fication earns its keep exactly when a pass draws
  them and a TTL reap ages them like everything else.
- the starfield → ONE entity with a Starfield component (positions,
  planes). Not 180 star entities: component granularity follows behavior
  granularity, and the field scrolls as one thing. No Element, no Order —
  every sim system and eachOrdered is structurally blind to it.

**What deliberately stays in the app shell:** window, audio, content,
pacer, input accumulators. Device handles and frame plumbing are not
world state; putting them in ctx is a service locator with extra steps.

**Stated invariant:** app-created entities in the shared registry change
entity-id allocation patterns. Harmless — id values feed nothing
gameplay-visible (review-004) and the replay digest does not fold them —
but recorded here so nobody later makes ids meaningful without noticing
the app churns them.

## 2. The stages

| Stage | What | Proof |
| --- | --- | --- |
| W1 | The semantic pass pipeline: draw() becomes the declared sequence above; Planet tag lands (gravityPass stops scanning by mass); CelPolicy retires, Visual shrinks to data; starfield/marks passes still read today's Game members (state moves in W2) | build clean; suite 8/8 untouched; replay baseline untouched (the Planet tag touches sim — gate hard); driven screenshots incl. the F1 overlay; deliberate stacking changes (ships over planet) named in the commit |
| W2 | The state migration: Starfield entity, Mark entities + age reap, AnnouncedDead tag, MatchState/debug/roster to Battle's typed ctx surface; Battle::destroy for app-owned entities | same gates + screenshots; Game struct visibly shrinks |
| W3 | Verdict; sim-architecture.md's app paragraph | the record |
