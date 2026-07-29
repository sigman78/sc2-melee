// Copyright the Ur-Quan Masters contributors. GPL-2.0-or-later.

#ifndef UQM2_SIM_BATTLE_HPP
#define UQM2_SIM_BATTLE_HPP

#include "engine/core/Types.hpp"
#include "sim/Damage.hpp"
#include "sim/Element.hpp"
#include "sim/Entity.hpp"
#include "sim/Field.hpp"
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
// Deterministic and self-contained: no wall clock, no I/O, no globals; the
// RNG is a battle member so a replay is exact.

// A read-only record of a contact between two entities this frame.
struct CollisionEvent
{
	// One side of the contact: who, and what the response did to it.
	struct Side
	{
		EntityId id = kNoEntity;

		// Velocities either side of the response, in world units per frame,
		// so a consumer can draw or measure the change without knowing the
		// fixed-point encoding.
		Vec2i before;
		Vec2i after;
	};

	Side a;
	Side b;

	// Where they met, in world units.
	Vec2i at;
};

// What SpawnEvent::kind distinguishes: the two flavors Sound.cpp's dispatch
// ever asks for, derived from composition at record time
// (Battle::recordSpawn), not stored on the spawned entity itself.
enum class SpawnFlavor : u8
{
	Unknown = 0,
	Weapon,
	Laser,
};

// Something entered the simulation this step; the audio layer reads these
// to start effects. Read-only, like CollisionEvent.
struct SpawnEvent
{
	EntityId id = kNoEntity;
	SpawnFlavor kind = SpawnFlavor::Unknown;
	i32 playerNr = -1;
};

// A spawn waiting for the sync point. The entity already exists and carries
// every component its caller attached -- only its Order is withheld, and
// that is what keeps it out of every pass until the frame ends. A pass that
// walks something other than Order joins Order to say so (Battle.cpp).
struct PendingSpawn
{
	EntityId id = kNoEntity;
	Layer layer = Layer::Field;

	// Set instead of `id` for a construction that cannot happen at emission
	// because it draws RNG, and the draw has to land at the sync point in
	// queue order (Field.cpp's rubble). The only one; a second appearing is
	// the signal to revisit this.
	void (*deferred)(
			Battle &, Borrowed<const CollisionMask>) noexcept = nullptr;
	Borrowed<const CollisionMask> deferredMask = nullptr;
};

class Spawned;

class Battle
{
public:
	explicit Battle(u32 seed);

	// The Order observers below hold `this`, so a Battle stays where it was
	// built.
	Battle(const Battle &) = delete;
	Battle &operator=(const Battle &) = delete;

	[[nodiscard]] Rng &rng() noexcept { return rng_; }

	// The generation check a stale handle fails. Declared order lives in
	// Order (Entity.hpp), read through eachOrdered below.
	[[nodiscard]] bool alive(EntityId id) const noexcept
	{
		return reg.valid(id);
	}
	// The Order pool is the element count: make() is the only thing that
	// attaches one, so nothing has to keep a parallel counter honest.
	[[nodiscard]] usize size() const noexcept
	{
		return reg.view<comp::Order>().size();
	}

	// CollidingElement (collide.h:31-33): a Collider, not Doomed, and not
	// WarpingIn -- a ship warping in keeps its Collider (ShipSystems.cpp)
	// but must not be hit, hence the WarpingIn exclusion.
	[[nodiscard]] bool collidable(EntityId id) const noexcept;

	// THE spawn: order is gameplay -- the caller names the stratum
	// (Entity.hpp Layer), FIFO within it.
	//
	// `collider` attaches a Collider iff non-null, and seeds PriorSilhouette
	// with the same value in the same call -- so the overlap-repair
	// protocol's first-frame comparison never reads a stale null.
	// `pos`, `motion`, `physique` default to zero: current/next/facing,
	// velocity, and mass all zero.
	// `allegiance` attaches uniformly on every spawn, no exceptions -- a
	// caller that doesn't care passes nothing and gets the default.
	//
	// Builds through make(), so it lands at the sync point when a step is in
	// flight; every further component is a `.with()` on the Spawned it
	// returns.
	Spawned spawn(Layer layer, comp::Position pos = comp::Position{},
			comp::Motion motion = comp::Motion{},
			comp::Physique physique = comp::Physique{},
			Borrowed<const CollisionMask> collider = nullptr,
			comp::Allegiance allegiance = comp::Allegiance{});

	// A beam's own spawn: no Position, Collider, Motion/Physique/
	// PriorSilhouette/CollisionScratch, or Appearing -- a beam never moves
	// or collides. Still gets Order and Allegiance, same as spawn().
	Spawned spawnBeam(Layer layer, comp::Beam beam,
			comp::Allegiance allegiance = comp::Allegiance{});

	// A decorative particle (ion trail, warp-in shadow, impact blast,
	// rubble): Position is set once and never touched again, so it carries
	// none of spawn()'s collision scaffold. Still gets Order and Allegiance.
	Spawned spawnEffect(Layer layer, comp::Position pos,
			comp::Allegiance allegiance = comp::Allegiance{});

	// The one decoration that actually drifts (the explosion's debris):
	// spawnEffect's shape plus Motion, so Integrate still advances it --
	// everything spawnEffect omits stays omitted.
	Spawned spawnEffect(Layer layer, comp::Position pos, comp::Motion motion,
			comp::Allegiance allegiance = comp::Allegiance{});

	// The fluent spawn: the entity, and every component the caller wants
	// named by a `.with()`. See Spawned below.
	//
	// Inside step() the Order is withheld until the sync point, so nothing
	// this frame can see, collide with or steer toward it, and the Order
	// pool stays still (ensureOrdered) -- the entity exists meanwhile, so
	// the caller attaches components directly instead of describing them.
	// Outside step() it lands at once, which is what setup wants: placement
	// has to see what it must not overlap.
	[[nodiscard]] Spawned make(Layer layer);

	// A bare entity: no Order, no components, outside size()'s element
	// count -- for app-owned world objects (starfield, marks) drawn by
	// their own pass, never walked by the sim's declared order.
	[[nodiscard]] EntityId create();

	// Destroys an app-owned entity. A sim element never comes through here:
	// it dies only through the Doomed/reap protocol, which runs its death
	// responses and keeps the count.
	void destroy(EntityId id) noexcept;

	// Defers a whole construction to the sync point, for the one case that
	// cannot be built at emission: spawnAsteroid draws RNG, and the draw
	// must land here in queue order (see PendingSpawn::deferred).
	void queueDeferred(
			void (*fn)(Battle &, Borrowed<const CollisionMask>) noexcept,
			Borrowed<const CollisionMask> mask);

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

	// The world. Public, and reached directly: emplace/try_get/get/all_of/
	// remove/view/ctx are entt's vocabulary, and wrapping it one API at a
	// time is what this replaced (review-010 §3). What stays on Battle is
	// what has semantics of its own -- the ordered walk, the spawn family,
	// and the two named component accessors below.
	entt::registry reg;

	// The ship component. Null for anything that is not a ship.
	[[nodiscard]] comp::ShipState *ship(EntityId id) noexcept;
	[[nodiscard]] const comp::ShipState *ship(EntityId id) const noexcept;
	comp::ShipState &attachShip(EntityId id, Borrowed<const ShipSpec> spec);

	// The spec a shot flies by, by value -- see FromWeapon. Use
	// `.with(FromWeapon{spec})` on the Spawned to attach one directly.
	[[nodiscard]] Borrowed<const WeaponSpec> weaponSpec(
			EntityId id) const noexcept;

	// The declared-order walk: the Order pool stays sorted ascending by
	// (Order.layer, Order.seq); ensureOrdered() re-sorts only when
	// something spawned or died since the last sort.
	//
	// One template, not two overloads: an explicit empty Ts pack is
	// otherwise ambiguous between them. Zero Ts walks bare ids;
	// eachOrdered<Ts...> joins, dropping entities missing any Ts.
	template <class... Ts, class Fn> void eachOrdered(Fn &&fn)
	{
		ensureOrdered();
		if constexpr (sizeof...(Ts) == 0)
		{
			for (EntityId const id : reg.view<comp::Order>())
				fn(id);
		}
		else
		{
			auto v = reg.view<comp::Order, Ts...>();
			v.template use<comp::Order>();
			v.each([&fn](EntityId id, comp::Order &, auto &...rest) {
				fn(id, rest...);
			});
		}
	}
	// The exclude form, matching view<Ts...>(excl)'s: an entity carrying any
	// of Xs is skipped before Ts is even checked.
	template <class... Ts, class... Xs, class Fn>
	void eachOrdered(entt::exclude_t<Xs...> excl, Fn &&fn)
	{
		ensureOrdered();
		if constexpr (sizeof...(Ts) == 0)
		{
			for (EntityId id : reg.view<comp::Order>(excl))
				fn(id);
		}
		else
		{
			auto v = reg.view<comp::Order, Ts...>(excl);
			v.template use<comp::Order>();
			v.each([&fn](EntityId id, comp::Order &, auto &...rest) {
				fn(id, rest...);
			});
		}
	}

private:
	// The batch pipeline: one function per slot, run in this fixed order
	// from step(). Later passes see earlier writes -- the sequence itself
	// is the ordering contract.
	void capturePriorPass() noexcept;
	void ageAndReapMarkPass();
	void animatePass();
	void integratePass() noexcept;
	void ageDecrementPass() noexcept;  // the decrement half of aging --
									   // runs after Integrate, before
									   // Collide; see the .cpp for why
	void collidePass();
	void applyDamageIncoming() noexcept;
	void reapPass() noexcept;
	void flagsEndOfFramePass() noexcept;  // must run before landPendingSpawns
										  // so a fresh spawn's own Appearing
										  // survives its first frame
	void landPendingSpawns();
	void commitPass() noexcept;

	// ProcessCollisions (process.c:361-627): walks candidates from `fromIdx`
	// in collideOrder_, a frame-sorted snapshot indexed (not linked) by
	// `elemIdx`/`fromIdx`. Returns whether `elem` ended the walk stopped.
	bool processCollisions(
			EntityId elem, usize elemIdx, usize fromIdx, TimeValue maxTime);
	bool resolveAgainst(EntityId elem, usize elemIdx, EntityId test,
			usize testIdx, TimeValue maxTime);
	void killOverlapSpawn(EntityId id);
	void recordSpawn(EntityId id, const comp::Allegiance &allegiance);
	void recordIfLanded(EntityId id, const comp::Allegiance &allegiance);

	// The death path's two mechanisms: the asteroid field's own cycle and
	// SweepsOwnedOnDeath's sweep of a dead ship's ordnance -- mutually
	// exclusive per entity, run from both death call sites.
	void runDeathResponses(EntityId id) noexcept;

	// Destroys the entity; the Order observer invalidates the sort.
	void removeElement(EntityId id) noexcept;

	// Re-sorts the Order pool by (layer, seq) iff a spawn or destroy has
	// touched it since the last sort -- the observers below raise the flag,
	// so a steady-state frame sorts nothing. Also reached from inside an
	// eachOrdered walk, so nothing may touch the pool between the top of
	// step() and the sync point.
	void ensureOrdered();

	// Connected to on_construct/on_destroy<Order> in the constructor: the
	// sort key's own pool says when the walk is stale, so no spawn or
	// destroy path has to remember to.
	void markOrderDirty(entt::registry &, EntityId) noexcept
	{
		orderDirty_ = true;
	}

	// The declared position every spawn gets: the caller's layer, FIFO
	// within it. One counter, one place it advances, so make() and the
	// spawn() family cannot drift apart on what seq means.
	[[nodiscard]] comp::Order nextOrder(Layer layer) noexcept;

	// Set true by markOrderDirty -- see ensureOrdered(). Starts true: an
	// empty pool has nothing to sort, but false would risk skipping the
	// first real one.
	bool orderDirty_ = true;
	Rng rng_;
	u64 frame_ = 0;
	u64 nextSeq_ = 0;

	// Both reused across steps so a steady-state frame allocates nothing.
	std::vector<CollisionEvent> collisions_;
	std::vector<SpawnEvent> spawns_;

	// Collide's own worklist: every live element, snapshotted and sorted
	// ascending by (Order.layer, Order.seq) at the top of collidePass,
	// addressable by index instead of by link. Rebuilt every frame.
	std::vector<EntityId> collideOrder_;

	// Entities built this frame, awaiting the Order that puts them in the
	// walk. Filled in pipeline order, landed in that same order.
	std::vector<PendingSpawn> pending_;

	// True from the top of step() until the sync point lands the frame's
	// spawns: what make() reads to decide whether an Order is attached now
	// or queued. False again during the landing itself, so a deferred
	// construction gets its Order in queue order like everything else.
	bool deferSpawns_ = false;

	friend class Spawned;
};

// A spawn under construction: Battle::make hands one back holding the
// new entity, `.with(...)` attaches a component value, and the whole
// chain converts to the id.
class Spawned
{
public:
	Spawned(Battle &b, EntityId id) noexcept : b_(b), id_(id) {}

	// A tag still reads as `.with(Appearing{})` at the call site: entt's
	// emplace has no value-carrying overload for an empty type, so this
	// picks the zero-arg attach instead.
	template <class T> Spawned &with(T &&value)
	{
		using U = std::decay_t<T>;
		if constexpr (std::is_empty_v<U>)
			b_.reg.emplace<U>(id_);
		else
			b_.reg.emplace<U>(id_, std::forward<T>(value));
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
