// Copyright the Ur-Quan Masters contributors. GPL-2.0-or-later.

#ifndef UQM2_SIM_BATTLE_HPP
#define UQM2_SIM_BATTLE_HPP

#include "engine/core/Types.hpp"
#include "sim/Damage.hpp"
#include "sim/Element.hpp"
#include "sim/Entity.hpp"
#include "sim/Random.hpp"
#include "sim/Ship.hpp"

#include <entt/entity/registry.hpp>

#include <optional>
#include <span>
#include <type_traits>
#include <utility>
#include <vector>

namespace uqm::sim {

// One battle's simulation state and its step: RedrawQueue (process.c:1013)
// with drawing removed -- shared by melee, hyperspace (hyper.c:1368) and
// interplanetary (ipdisp.c:567).
//
// Deterministic and self-contained: no wall clock, no I/O, no globals: the
// RNG is a battle member so a replay is exact -- see design-notes V8.

// What happened when two things touched. Observational only -- see
// design-notes D5.
struct CollisionEvent
{
	EntityId a = kNoEntity;
	EntityId b = kNoEntity;

	// Where they met, in world units.
	Vec2i at;

	// Velocities either side of the response, in world units per frame, so a
	// consumer can draw or measure the change without knowing the fixed-point
	// encoding.
	Vec2i beforeA;
	Vec2i beforeB;
	Vec2i afterA;
	Vec2i afterB;
};

// Something entered the simulation this step; the audio layer reads these to
// start effects. Observational only, like CollisionEvent -- see design-notes D5.
struct SpawnEvent
{
	EntityId id = kNoEntity;
	ElementKind kind = ElementKind::Unknown;
	i32 playerNr = -1;
};

// A spawn requested mid-frame: enters the world at Battle::step's sync
// point, not the instant a pipeline pass asks for it (review-006 §2's
// command buffer) -- so a shot fired, a blast lit, or a trail dropped this
// frame does not exist for anything else this same frame to see, collide
// with, or steer toward (review-006 §4's accepted one-frame latency).
//
// Deliberately not a generic attach-anything mechanism: just the handful of
// tag/optional components a queued spawn actually needs today (review-006
// §7 declines a general dispatch mechanism for the same reason).
struct SpawnCommand
{
	Layer layer = Layer::Field;
	Element element;
	Position position;
	Motion motion;
	Physique physique;

	// Passed through to spawn()/spawnBeam() unchanged, same reason it is
	// uniform there: every queued spawn gets one, no exceptions.
	Allegiance allegiance;

	// Set only for a beam: routes the drain through Battle::spawnBeam
	// instead of Battle::spawn, so `position` above is never even read for
	// one -- a beam has no Position at all (review-007 W4a).
	std::optional<Beam> beam;

	// Set for a decorative particle -- an ion trail, a warp-in shadow, an
	// impact blast, rubble -- routing the drain through Battle::spawnEffect
	// instead of Battle::spawn: none of the collision scaffold
	// (Physique/PriorSilhouette/CollisionScratch/Collider) is ever read for
	// one (review-007 W5's diet). `effectMoves` additionally attaches
	// Motion, for the one decoration that actually drifts (the explosion's
	// debris) -- every other decoration's Position is set once at spawn and
	// never touched again.
	bool effect = false;
	bool effectMoves = false;

	// Non-null for a weapon: attachWeaponSpec's payload.
	Borrowed<const WeaponSpec> weaponSpec = nullptr;

	// Set for a guided weapon; the clock inside is already wound (see
	// ShipSystems.cpp's fire block).
	std::optional<Guided> guided;

	bool ignoreSimilar = false;

	// Set for a transient spawn: Lifetime attaches once the spawn lands.
	// Element lost `lifeSpan` (review-007 W3), so a queued FiniteLife shot,
	// trail, blast or spark carries its countdown here instead.
	std::optional<Lifetime> lifetime;

	// Set only for a weapon shot (the fire block): Vitality attaches once
	// the spawn lands. Element lost `hitPoints` the same way it lost
	// `lifeSpan` (review-007 W4b) -- attached where read, not uniformly.
	std::optional<Vitality> vitality;

	// Set only for a weapon shot: Warhead attaches once the spawn lands.
	// Element lost `damage`/`blastOffset` the same way (review-007 W4b).
	std::optional<Warhead> warhead;

	// Set only for a weapon shot: AnimFrame attaches once the spawn lands.
	// Element lost `colorCycle` the same way (review-007 W4b).
	std::optional<AnimFrame> animFrame;

	// True stamps FrameDriven onto the shot (WeaponSpec::frameDriven):
	// true only for the flame, whose animate-pass sub-iteration this tag
	// selects (review-007 W5).
	bool frameDriven = false;

	// Non-null attaches a Collider once the spawn lands (the fire block's
	// shot; see Battle::spawn, which every direct spawn site goes through
	// instead).
	Borrowed<const CollisionMask> collider = nullptr;

	// Non-null for the asteroid field's rubble (Field.cpp's asteroidDeath):
	// the mask it carries through its own non-solid life, so its own death
	// spawn can hand it to the next asteroid -- never a Collider, since that
	// would make the rubble collide. See StashedMask (Collision.hpp).
	Borrowed<const CollisionMask> rubbleMask = nullptr;

	// Non-null attaches a DeathSpawn once the spawn lands (Field.hpp): the
	// rubble's own payload (rubbleDeath), matching the asteroid's own
	// (Field.cpp's spawnAsteroid attaches DeathSpawn directly, having no
	// need for the queue). Same signature as DeathSpawn::emit.
	void (*deathSpawn)(Battle &, EntityId) noexcept = nullptr;

	// An escape hatch for a spawn whose construction itself must happen at
	// the sync point, in queue order, instead of at emission -- the
	// asteroid field's recycle (Field.cpp's rubbleDeath) draws RNG building
	// its Element and attaches a Spin component; drawing that RNG at
	// emission (its trigger's onDeath, mid-pipeline) would put it out of
	// step with everything else the sync point still has to draw or
	// build. When set, every field above is ignored: `deferred` gets a
	// full Battle& and `deferredMask` and does its own spawn() (and
	// whatever else it needs).
	void (*deferred)(Battle &, Borrowed<const CollisionMask>) noexcept = nullptr;
	Borrowed<const CollisionMask> deferredMask = nullptr;
};

class Spawned;

class Battle
{
public:
	explicit Battle(u32 seed);

	[[nodiscard]] Rng &rng() noexcept { return rng_; }

	// alive() is the generation check a stale handle fails; declared order
	// lives in Order (Entity.hpp) and is read through eachOrdered below, not
	// walked link by link (review-006 Z6 retired the OrderLink spine).
	[[nodiscard]] bool alive(EntityId id) const noexcept
	{
		return reg_.valid(id);
	}
	[[nodiscard]] usize size() const noexcept { return count_; }

	// CollidingElement (collide.h:31-33): a Collider and not Doomed.
	// Something dying this frame must not still be hit, and must not still
	// pull on anything. Free-standing rather than an Element method now that
	// solidity lives in a separate component (review-007 W2).
	[[nodiscard]] bool collidable(EntityId id) const noexcept;

	// The element, or null for a dead or stale id. A raw borrow: the pool
	// is in_place_delete, so the address holds for the entity's lifetime --
	// but the old EntityRef's removed-while-held debug check is gone with
	// EntityList (review-004's ledger records the loss).
	[[nodiscard]] Element *get(EntityId id) noexcept
	{
		return reg_.valid(id) ? reg_.try_get<Element>(id) : nullptr;
	}
	[[nodiscard]] const Element *get(EntityId id) const noexcept
	{
		return reg_.valid(id) ? reg_.try_get<Element>(id) : nullptr;
	}

	// THE spawn: order is gameplay (design-notes D8), and the position is
	// declared, not computed -- the caller names the stratum (Entity.hpp
	// Layer), FIFO within it. spawnFront/spawnBack/insertAfter retired
	// with review-005 Y2; the C's InsertElement gymnastics (pkunk.c:498-512
	// head-inserts the phoenix to preprocess before the death hook) become
	// a Layer declaration when that ship arrives.
	//
	// `collider` attaches a Collider iff non-null, and seeds PriorSilhouette
	// with the same value in the same call -- centralised here so no caller
	// can attach one a statement late and leave the overlap-repair protocol's
	// first-frame comparison reading a stale null (see the .cpp).
	// `pos`, `motion` and `physique` default to Position{}/Motion{}/
	// Physique{} -- current/next/facing, velocity and mass all zero, same as
	// Element's own fields defaulted before the split (review-007 W4a).
	// `allegiance` attaches uniformly, every spawn, no exceptions (review-007
	// W4b: the one deliberately-uniform component of this stage) -- its own
	// default matches Element::playerNr/owner's old defaults exactly, so a
	// caller that doesn't care passes nothing.
	EntityId spawn(Layer layer, Element e, Position pos = Position{},
			Motion motion = Motion{}, Physique physique = Physique{},
			Borrowed<const CollisionMask> collider = nullptr,
			Allegiance allegiance = Allegiance{});

	// A beam's own spawn (review-007 W4a, diet W4b): no Position, no
	// Collider, no Motion/Physique/PriorSilhouette/CollisionScratch, no
	// Appearing -- a beam never moves and never collides, so it carries
	// none of the scaffold those imply (the collide pass gates on
	// collidable(testId) before it would ever read through the hole);
	// `beam` is what a mover's `pos` is elsewhere. Still gets Order (walked
	// and drawn like anything else) and Allegiance, same as spawn() above.
	EntityId spawnBeam(Layer layer, Element e, Beam beam,
			Allegiance allegiance = Allegiance{});

	// A decorative particle -- an ion trail, a warp-in shadow, an impact
	// blast, rubble -- queued via SpawnCommand's `effect` flag (review-007
	// W5's diet): never solid and its Position is set once at spawn and
	// never touched again, so it carries none of spawn()'s collision
	// scaffold either (Motion/Physique/PriorSilhouette/CollisionScratch/
	// Collider -- the collide pass gates on collidable(testId), which this
	// never satisfies). Still gets Order and Allegiance, same as spawn()
	// and spawnBeam above.
	EntityId spawnEffect(Layer layer, Element e, Position pos,
			Allegiance allegiance = Allegiance{});

	// The one decoration that actually drifts (the explosion's debris):
	// spawnEffect's shape plus Motion, so Integrate still advances it --
	// everything spawnEffect omits stays omitted.
	EntityId spawnEffect(Layer layer, Element e, Position pos, Motion motion,
			Allegiance allegiance = Allegiance{});

	// The fluent spawn (review-007 W9): the entity with its declared Order
	// and nothing else, every component the caller wants named by a .with().
	// See Spawned below.
	[[nodiscard]] Spawned make(Layer layer);

	// A bare entity: no Order, no components, outside size()'s element count
	// -- what the app builds its own world objects from (review-007 §3's
	// starfield and marks), which are drawn by their own pass and never
	// walked by the sim's declared order. Anything the sim steps is spawned
	// instead.
	[[nodiscard]] EntityId create();

	// Destroys an app-owned entity. A sim element never comes through here:
	// it dies only through the Doomed/reap protocol, which runs its death
	// responses and keeps the count.
	void destroy(EntityId id) noexcept;

	// Registers a spawn for the sync point instead of creating it now --
	// what a pipeline pass calls in place of spawn() (see SpawnCommand).
	void queueSpawn(SpawnCommand cmd);

	// One simulation step, 1/24 second of game time.
	void step();

	[[nodiscard]] u64 frame() const noexcept { return frame_; }

	// Collisions resolved during the most recent step. Cleared at the start of
	// each one, so this is always the current frame's.
	[[nodiscard]] std::span<const CollisionEvent> collisions() const noexcept
	{
		return collisions_;
	}

	// Elements spawned during the most recent step, in spawn order. Cleared
	// at the start of each step, so setup spawns are not reported.
	[[nodiscard]] std::span<const SpawnEvent> spawns() const noexcept
	{
		return spawns_;
	}

	// Components are registry pools keyed by the entity (review-002 §1,
	// review-004 X3): attach is emplace, lookup is try_get, and destroy
	// reaps every component with its entity -- the old sidecar vectors,
	// their linear finds and dropComponents are gone.

	// The ship component. Null for anything that is not a ship.
	[[nodiscard]] ShipState *ship(EntityId id) noexcept;
	[[nodiscard]] const ShipState *ship(EntityId id) const noexcept;
	ShipState &attachShip(EntityId id, Borrowed<const ShipSpec> spec);

	// The weapon-guidance component, by value -- see WeaponGuidance.
	[[nodiscard]] Borrowed<const WeaponSpec> weaponSpec(
			EntityId id) const noexcept;
	void attachWeaponSpec(EntityId id, Borrowed<const WeaponSpec> spec);

	// The typed component surface: every component type the sim or the app
	// defines goes through these instead of naming entt::registry directly --
	// review-004 open question 3's ownership-by-component-type answer, closed.
	// decltype(auto), not T&: entt's emplace returns void for an empty
	// (tag) component -- PlayerShip, WarpingIn, Exploding -- since there is
	// nothing to reference; no call site uses the return value for those.
	template <class T, class... Args>
	decltype(auto) attach(EntityId id, Args &&...args)
	{
		return reg_.emplace<T>(id, std::forward<Args>(args)...);
	}
	// For a site that cannot promise the component is absent -- a kill-now
	// path retargeting an entity that may already carry one (a weapon mid-
	// flight already has a Lifetime; doDamage's non-ship branch has to
	// overwrite it, not assert on it).
	template <class T, class... Args>
	decltype(auto) attachOrReplace(EntityId id, Args &&...args)
	{
		return reg_.emplace_or_replace<T>(id, std::forward<Args>(args)...);
	}
	template <class T>
	[[nodiscard]] T *find(EntityId id) noexcept
	{
		return reg_.valid(id) ? reg_.try_get<T>(id) : nullptr;
	}
	template <class T>
	[[nodiscard]] const T *find(EntityId id) const noexcept
	{
		return reg_.valid(id) ? reg_.try_get<T>(id) : nullptr;
	}
	template <class T>
	[[nodiscard]] bool has(EntityId id) const noexcept
	{
		return reg_.valid(id) && reg_.all_of<T>(id);
	}
	template <class T>
	void detach(EntityId id)
	{
		reg_.remove<T>(id);
	}

	// The singleton surface (review-007 §3): one value per type, belonging to
	// the world rather than to any entity -- what an app-side member reached
	// around for when a pass needed the camera, the match state or the
	// config. Same rule as the component surface above: entt::registry never
	// escapes Battle.
	template <class T>
	T &setContext(T v)
	{
		return reg_.ctx().insert_or_assign(std::move(v));
	}
	template <class T>
	[[nodiscard]] T &context()
	{
		return reg_.ctx().get<T>();
	}
	template <class T>
	[[nodiscard]] const T &context() const
	{
		return reg_.ctx().get<T>();
	}
	// For a reader that runs before the value is installed, or when its
	// absence is itself the answer.
	template <class T>
	[[nodiscard]] T *findContext() noexcept
	{
		return reg_.ctx().find<T>();
	}
	template <class T>
	[[nodiscard]] const T *findContext() const noexcept
	{
		return reg_.ctx().find<T>();
	}

	// The typed join (review-007 W4b's join rule): a pass's component set is
	// its call signature now, not documentation above a blanket get<> or a
	// find<T> per iteration -- reg_.view<Ts...>().each(fn) yielding
	// (EntityId, Ts&...), order-free (for the scans that don't need the
	// spine -- Field.cpp's gravity/placement checks). entt::registry still
	// never escapes Battle. Ts always explicit at the call site (each<Ts...>
	// isn't deducible from Fn alone); a tag among Ts still filters presence
	// without the callback needing a reference for it -- entt elides an
	// empty type from the yielded tuple on its own.
	template <class... Ts, class Fn>
	void each(Fn &&fn)
	{
		reg_.view<Ts...>().each(std::forward<Fn>(fn));
	}
	template <class... Ts, class Fn>
	void each(Fn &&fn) const
	{
		reg_.view<const Ts...>().each(std::forward<Fn>(fn));
	}
	// The exclude form: presence/absence is a query, not an in-body
	// has<X>/!has<X> guard (SiGMan's review) -- an entity carrying any of
	// Xs never reaches the callback at all.
	template <class... Ts, class... Xs, class Fn>
	void each(entt::exclude_t<Xs...> excl, Fn &&fn)
	{
		reg_.view<Ts...>(excl).each(std::forward<Fn>(fn));
	}
	template <class... Ts, class... Xs, class Fn>
	void each(entt::exclude_t<Xs...> excl, Fn &&fn) const
	{
		reg_.view<const Ts...>(excl).each(std::forward<Fn>(fn));
	}

	// The declared-order walk (review-006 Z6): a local scratch of every live
	// id, sorted ascending by (Order.layer, Order.seq) -- rebuilt fresh per
	// call (a battle is ~40 entities; no caching). collidePass builds the
	// same scratch through the private helper below instead of duplicating
	// the sort.
	//
	// One template, not two overloads: a bare eachOrdered(fn) call (no
	// explicit Ts) and an eachOrdered<Ts...>(fn) call both bind here, since
	// an unused trailing pack deduces to empty on its own -- two separate
	// templates for the same one-argument shape left the empty-Ts case
	// genuinely ambiguous between them (found by the compiler, not by
	// inspection). The zero-Ts form is the walk that genuinely wants bare
	// ids; eachOrdered<Ts...> is the join -- fetch every Ts and skip the
	// entity if any is missing (reg_.all_of first) rather than passing a
	// null through. Ts always explicit when non-empty (it isn't deducible
	// from Fn alone).
	template <class... Ts, class Fn>
	void eachOrdered(Fn &&fn)
	{
		std::vector<EntityId> ids;
		buildOrderedIds(ids);
		for (EntityId id : ids)
		{
			if constexpr (sizeof...(Ts) == 0)
				fn(id);
			else if (reg_.all_of<Ts...>(id))
				fn(id, reg_.get<Ts>(id)...);
		}
	}
	template <class... Ts, class Fn>
	void eachOrdered(Fn &&fn) const
	{
		std::vector<EntityId> ids;
		buildOrderedIds(ids);
		for (EntityId id : ids)
		{
			if constexpr (sizeof...(Ts) == 0)
				fn(id);
			else if (reg_.all_of<Ts...>(id))
				fn(id, reg_.get<Ts>(id)...);
		}
	}
	// The exclude form, matching each<Ts...>'s: an entity carrying any of
	// Xs is skipped before Ts is even checked.
	template <class... Ts, class... Xs, class Fn>
	void eachOrdered(entt::exclude_t<Xs...>, Fn &&fn)
	{
		std::vector<EntityId> ids;
		buildOrderedIds(ids);
		for (EntityId id : ids)
			if (reg_.all_of<Ts...>(id) && !reg_.any_of<Xs...>(id))
				fn(id, reg_.get<Ts>(id)...);
	}

private:
	// The world itself, for component types the typed surface above does not
	// cover. Private: everything outside Battle goes through attach/find/
	// has/detach instead (review-004 open question 3).
	[[nodiscard]] entt::registry &registry() noexcept { return reg_; }
	[[nodiscard]] const entt::registry &registry() const noexcept
	{
		return reg_;
	}

	// The batch pipeline (review-006-batch.md §1, §6 Z4): one function per
	// slot, run in this fixed order from step(). Later passes see earlier
	// writes -- the sequence itself is the ordering contract.
	void capturePriorPass() noexcept;    // 1 CapturePrior
	void ageAndReapMarkPass();           // 2 AgeAndReap-mark (the check)
	void animatePass();                  // 7 Animate
	void integratePass() noexcept;       // 8 Integrate
	void ageDecrementPass() noexcept;    // the decrement half of aging --
	                                      // runs after Integrate, before
	                                      // Collide; see the .cpp for why
	void collidePass();                  // 9 Collide
	void applyDamageIncoming() noexcept; // 11a
	void reapPass() noexcept;            // 11c
	void flagsEndOfFramePass() noexcept; // 11e (run before 11d -- see .cpp)
	void drainSpawnCommands();           // 11d
	void commitPass() noexcept;          // 12 Commit

	// ProcessCollisions (process.c:361-627): walks candidates from `fromIdx`
	// in collideOrder_ (Z5: an index into that frame-sorted snapshot, not a
	// spine link -- see collidePass). `elemIdx` is the scanner's own fixed
	// position in the same snapshot. Returns whether `elem` ended the walk
	// stopped. No longer preprocesses stragglers (Z4: integration is a
	// whole-spine pass before this one ever runs, so there is nothing left
	// unintegrated to catch).
	bool processCollisions(
			EntityId elem, usize elemIdx, usize fromIdx, TimeValue maxTime);
	bool resolveAgainst(EntityId elem, usize elemIdx, EntityId test,
			usize testIdx, TimeValue maxTime);
	void killOverlapSpawn(EntityId id);
	void recordSpawn(EntityId id, const Element &e, const Allegiance &allegiance);

	// The death path's two mechanisms (review-007 W5, replacing
	// Element::onDeath): DeathSpawn's payload (asteroid/rubble) and
	// SweepsOwnedOnDeath's generic sweep (a dying ship's ordnance) --
	// mutually exclusive per entity, called from both death call sites
	// (killOverlapSpawn's mid-Collide kill and ageAndReapMarkPass's
	// frame-start check).
	void runDeathResponses(EntityId id) noexcept;

	// ex-EntityList::remove (review-006 Z6): destroy plus the count, no
	// spine to unlink.
	void removeElement(EntityId id) noexcept;

	// Shared by eachOrdered and collidePass: every live element's id,
	// ascending by (Order.layer, Order.seq).
	void buildOrderedIds(std::vector<EntityId> &out) const noexcept;

	// The declared position every spawn gets: the caller's layer, FIFO
	// within it. One counter, one place it advances, so make() and the
	// spawn() family cannot drift apart on what seq means.
	[[nodiscard]] Order nextOrder(Layer layer) noexcept;

	entt::registry reg_;
	usize count_ = 0;
	Rng rng_;
	u64 frame_ = 0;
	u64 nextSeq_ = 0;

	// Both reused across steps so a steady-state frame allocates nothing.
	std::vector<CollisionEvent> collisions_;
	std::vector<SpawnEvent> spawns_;

	// Collide's own worklist (Z5): every live element, snapshotted and
	// sorted ascending by (Order.layer, Order.seq) at the top of collidePass,
	// addressable by index instead of by link. Reused across steps like the
	// vectors above; cleared and rebuilt every frame since Collide runs
	// after this frame's Integrate.
	std::vector<EntityId> collideOrder_;

	// The command buffer (review-006 §2): filled in pipeline order by
	// queueSpawn, drained in that same order at the sync point.
	std::vector<SpawnCommand> spawnCommands_;

	friend class Spawned;
};

// A spawn under construction (review-007 W9): Battle::make hands one back
// holding the new entity, `.with(...)` attaches a component value, and the
// whole chain converts to the id -- so a spawn site reads as the list of
// components the thing is made of, in place of a positional argument list
// that had to name every component any spawn might want.
class Spawned
{
public:
	Spawned(Battle &b, EntityId id) noexcept : b_(b), id_(id) {}

	template <class T>
	Spawned &with(T &&value)
	{
		b_.attach<std::decay_t<T>>(id_, std::forward<T>(value));
		return *this;
	}

	[[nodiscard]] EntityId id() const noexcept { return id_; }
	operator EntityId() const noexcept { return id_; }

private:
	Battle &b_;
	EntityId id_;
};

}  // namespace uqm::sim

#endif  // UQM2_SIM_BATTLE_HPP
