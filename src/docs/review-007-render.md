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

**Rendering becomes a declared pipeline**, symmetric to step():

    clear → Starfield → RenderLayer(Background) → RenderLayer(Field)
          → RenderLayer(Ordnance) → Marks → Hud → Overlay(ctx-gated)
          → present

Each RenderLayer pass walks its stratum in (layer, seq) order and
dispatches on Visual.policy per entity. Decomposition is by *layer*, not
by technique: per-technique passes (view<SpriteVisual>, view<BeamVisual>…)
would batch draws across seq and break z-order within a layer, and at
forty entities there is no batching win to pay for that. Visual's policy
enum is data-driven dispatch already; new layers becoming new passes is
the extensibility that matters (M2 panels, hyperspace strata).

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
| W1 | draw() decomposes into the declared pass list; per-layer element passes; hud/overlay/marks/starfield as named passes (marks/starfield still reading today's Game members — state moves in W2) | build clean; suite 8/8 untouched; replay baseline untouched; driven screenshots incl. the F1 overlay |
| W2 | The state migration: Starfield entity, Mark entities + age reap, AnnouncedDead tag, MatchState/debug/roster to Battle's typed ctx surface; Battle::destroy for app-owned entities | same gates + screenshots; Game struct visibly shrinks |
| W3 | Verdict; sim-architecture.md's app paragraph | the record |
