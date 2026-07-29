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

// A spawn requested mid-frame enters the world at Battle::step's sync
// point, not when a pipeline pass asks for it: it doesn't exist for
// anything else that frame to see, collide with, or steer toward.
struct SpawnCommand
{
	Layer layer = Layer::Field;
	Position position;
	Motion motion;
	Physique physique;

	// Passed through to spawn()/spawnBeam() unchanged, same reason it is
	// uniform there: every queued spawn gets one, no exceptions.
	Allegiance allegiance;

	// Set only for a beam: routes the drain through Battle::spawnBeam
	// instead of Battle::spawn -- a beam carries no Position, so
	// `position` above is never read for one.
	std::optional<Beam> beam;

	// Set for a decorative particle (ion trail, warp-in shadow, impact
	// blast, rubble): routes the drain through Battle::spawnEffect instead
	// of Battle::spawn, carrying none of the collision scaffold.
	bool effect = false;
	// Additionally attaches Motion, for the one decoration that drifts
	// (explosion debris); other decorations' Position is set once and
	// never touched again.
	bool effectMoves = false;

	// Which render tag lands on an effect spawn: at most one is ever true,
	// set by the effect's own spawn site (spawnIonTrail, warpInStep,
	// explosionStep, the blast sites in Field.cpp/Damage.cpp).
	bool trail = false;
	bool shadow = false;
	bool debris = false;
	bool blast = false;

	// Non-null for a weapon: attaches a FromWeapon once the spawn lands.
	Borrowed<const WeaponSpec> weaponSpec = nullptr;

	// Set for a guided weapon; the clock inside is already wound (see
	// ShipSystems.cpp's fire block).
	std::optional<Guided> guided;

	bool ignoreSimilar = false;

	// Set for a transient spawn: Lifetime attaches once the spawn lands,
	// carrying the countdown for a queued FiniteLife shot, trail, blast,
	// or spark.
	std::optional<Lifetime> lifetime;

	// Set only for a weapon shot (the fire block): Vitality attaches once
	// the spawn lands, not uniformly -- attached only where it's read.
	std::optional<Vitality> vitality;

	// Set only for a weapon shot: passed to Battle::spawn, which attaches
	// it before the spawn is recorded -- recordSpawn derives Weapon flavor
	// from has<Warhead>, so this must already be in place by then.
	std::optional<Warhead> warhead;

	// Set only for a weapon shot: AnimFrame attaches once the spawn lands.
	std::optional<AnimFrame> animFrame;

	// True stamps FrameDriven onto the shot (WeaponSpec::frameDriven):
	// only the flame sets it, selecting animate-pass's sub-iteration.
	bool frameDriven = false;

	// Non-null attaches a Collider once the spawn lands (the fire block's
	// shot); every direct spawn site goes through Battle::spawn instead.
	Borrowed<const CollisionMask> collider = nullptr;

	// Non-null for the asteroid field's rubble (Field.cpp's asteroidDeath):
	// carries the mask through its non-solid life for the next asteroid's
	// death spawn -- never a Collider, or the rubble would collide.
	Borrowed<const CollisionMask> rubbleMask = nullptr;

	// Non-null attaches a DeathSpawn once the spawn lands (Field.hpp): the
	// rubble's own payload, matching DeathSpawn::emit's signature. The
	// asteroid's own payload attaches directly in spawnAsteroid instead.
	void (*deathSpawn)(Battle &, EntityId) noexcept = nullptr;

	// Escape hatch for a spawn whose construction (e.g. RNG draws) must
	// happen at the sync point, not at emission (Field.cpp's rubbleDeath).
	// When set, every field above is ignored; deferred does its own spawn().
	void (*deferred)(
			Battle &, Borrowed<const CollisionMask>) noexcept = nullptr;
	Borrowed<const CollisionMask> deferredMask = nullptr;
};

class Spawned;

class Battle
{
public:
	explicit Battle(u32 seed);

	[[nodiscard]] Rng &rng() noexcept { return rng_; }

	// The generation check a stale handle fails. Declared order lives in
	// Order (Entity.hpp), read through eachOrdered below.
	[[nodiscard]] bool alive(EntityId id) const noexcept
	{
		return reg_.valid(id);
	}
	[[nodiscard]] usize size() const noexcept { return count_; }

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
	// `warhead` attaches iff set, centralised here (not a statement later)
	// because recordSpawn's flavor derivation needs it already attached.
	//
	// Builds through make(): the Spawned it returns can still take further
	// `.with()` calls (spawnPlayerShip's IgnoreSimilar, per-command extras
	// in drainSpawnCommands).
	Spawned spawn(Layer layer, Position pos = Position{},
			Motion motion = Motion{}, Physique physique = Physique{},
			Borrowed<const CollisionMask> collider = nullptr,
			Allegiance allegiance = Allegiance{},
			std::optional<Warhead> warhead = std::nullopt);

	// A beam's own spawn: no Position, Collider, Motion/Physique/
	// PriorSilhouette/CollisionScratch, or Appearing -- a beam never moves
	// or collides. Still gets Order and Allegiance, same as spawn().
	Spawned spawnBeam(
			Layer layer, Beam beam, Allegiance allegiance = Allegiance{});

	// A decorative particle (ion trail, warp-in shadow, impact blast,
	// rubble): Position is set once and never touched again, so it carries
	// none of spawn()'s collision scaffold. Still gets Order and Allegiance.
	Spawned spawnEffect(
			Layer layer, Position pos, Allegiance allegiance = Allegiance{});

	// The one decoration that actually drifts (the explosion's debris):
	// spawnEffect's shape plus Motion, so Integrate still advances it --
	// everything spawnEffect omits stays omitted.
	Spawned spawnEffect(Layer layer, Position pos, Motion motion,
			Allegiance allegiance = Allegiance{});

	// The fluent spawn: the entity with its declared Order and nothing
	// else; every component the caller wants is named by a .with().
	// See Spawned below.
	[[nodiscard]] Spawned make(Layer layer);

	// A bare entity: no Order, no components, outside size()'s element
	// count -- for app-owned world objects (starfield, marks) drawn by
	// their own pass, never walked by the sim's declared order.
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

	// Components are registry pools keyed by the entity: attach is emplace,
	// lookup is try_get, destroy reaps every component with its entity.

	// The ship component. Null for anything that is not a ship.
	[[nodiscard]] ShipState *ship(EntityId id) noexcept;
	[[nodiscard]] const ShipState *ship(EntityId id) const noexcept;
	ShipState &attachShip(EntityId id, Borrowed<const ShipSpec> spec);

	// The spec a shot flies by, by value -- see FromWeapon. Use
	// `.with(FromWeapon{spec})` on the Spawned to attach one directly.
	[[nodiscard]] Borrowed<const WeaponSpec> weaponSpec(
			EntityId id) const noexcept;

	// The typed component surface: every component type goes through
	// these instead of naming entt::registry directly.
	// decltype(auto), not T&: entt's emplace returns void for an empty
	// (tag) component -- WarpingIn, Exploding -- since there is nothing to
	// reference.
	template <class T, class... Args>
	decltype(auto) attach(EntityId id, Args &&...args)
	{
		return reg_.emplace<T>(id, std::forward<Args>(args)...);
	}
	// For a site that cannot promise the component is absent -- e.g. a
	// weapon mid-flight already has a Lifetime, and doDamage's non-ship
	// branch must overwrite it rather than assert.
	template <class T, class... Args>
	decltype(auto) attachOrReplace(EntityId id, Args &&...args)
	{
		return reg_.emplace_or_replace<T>(id, std::forward<Args>(args)...);
	}
	template <class T> [[nodiscard]] T *find(EntityId id) noexcept
	{
		return reg_.valid(id) ? reg_.try_get<T>(id) : nullptr;
	}
	template <class T> [[nodiscard]] const T *find(EntityId id) const noexcept
	{
		return reg_.valid(id) ? reg_.try_get<T>(id) : nullptr;
	}
	// Asserts every Ts is attached and returns references (a tuple for
	// several) -- for a site where presence is already guaranteed. find<T>
	// stays the null-returning form for a site that must still ask.
	template <class... Ts> [[nodiscard]] decltype(auto) get(EntityId id)
	{
		return reg_.get<Ts...>(id);
	}
	template <class T> [[nodiscard]] bool has(EntityId id) const noexcept
	{
		return reg_.valid(id) && reg_.all_of<T>(id);
	}
	template <class T> void detach(EntityId id) { reg_.remove<T>(id); }

	// The singleton surface: one value per type, belonging to the world
	// rather than to any entity (camera, match state, config). Same rule
	// as the component surface: entt::registry never escapes Battle.
	template <class T> T &setContext(T v)
	{
		return reg_.ctx().insert_or_assign(std::move(v));
	}
	template <class T> [[nodiscard]] T &context()
	{
		return reg_.ctx().get<T>();
	}
	template <class T> [[nodiscard]] const T &context() const
	{
		return reg_.ctx().get<T>();
	}
	// For a reader that runs before the value is installed, or when its
	// absence is itself the answer.
	template <class T> [[nodiscard]] T *findContext() noexcept
	{
		return reg_.ctx().find<T>();
	}
	template <class T> [[nodiscard]] const T *findContext() const noexcept
	{
		return reg_.ctx().find<T>();
	}

	// entt's own view vocabulary, not a hand-rolled fraction of it: the
	// caller gets iterators, use<>, size_hint and storage through the view.
	// entt::registry itself never escapes Battle -- only the view does.
	template <class... Ts> [[nodiscard]] auto view()
	{
		return reg_.view<Ts...>();
	}
	template <class... Ts> [[nodiscard]] auto view() const
	{
		return reg_.view<const Ts...>();
	}
	template <class... Ts, class... Xs>
	[[nodiscard]] auto view(entt::exclude_t<Xs...> excl)
	{
		return reg_.view<Ts...>(excl);
	}
	template <class... Ts, class... Xs>
	[[nodiscard]] auto view(entt::exclude_t<Xs...> excl) const
	{
		return reg_.view<const Ts...>(excl);
	}

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
			for (EntityId const id : reg_.view<Order>())
				fn(id);
		}
		else
		{
			auto v = reg_.view<Order, Ts...>();
			v.template use<Order>();
			v.each([&fn](EntityId id, Order &, auto &...rest) {
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
			for (EntityId id : reg_.view<Order>(excl))
				fn(id);
		}
		else
		{
			auto v = reg_.view<Order, Ts...>(excl);
			v.template use<Order>();
			v.each([&fn](EntityId id, Order &, auto &...rest) {
				fn(id, rest...);
			});
		}
	}

	// The same sorted-and-driven view eachOrdered iterates internally,
	// handed back instead of walked -- for a caller that wants entt's own
	// iterator/size_hint surface over the declared order.
	template <class... Ts> [[nodiscard]] auto ordered()
	{
		ensureOrdered();
		auto v = reg_.view<Order, Ts...>();
		v.template use<Order>();
		return v;
	}

private:
	// The world itself, for component types the typed surface above does
	// not cover. Private: everything outside Battle goes through
	// attach/find/has/detach instead.
	[[nodiscard]] entt::registry &registry() noexcept { return reg_; }
	[[nodiscard]] const entt::registry &registry() const noexcept
	{
		return reg_;
	}

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
	void flagsEndOfFramePass() noexcept;  // must run before drainSpawnCommands
										  // so a fresh spawn's own Appearing
										  // survives its first frame
	void drainSpawnCommands();
	void commitPass() noexcept;

	// ProcessCollisions (process.c:361-627): walks candidates from `fromIdx`
	// in collideOrder_, a frame-sorted snapshot indexed (not linked) by
	// `elemIdx`/`fromIdx`. Returns whether `elem` ended the walk stopped.
	bool processCollisions(
			EntityId elem, usize elemIdx, usize fromIdx, TimeValue maxTime);
	bool resolveAgainst(EntityId elem, usize elemIdx, EntityId test,
			usize testIdx, TimeValue maxTime);
	void killOverlapSpawn(EntityId id);
	void recordSpawn(EntityId id, const Allegiance &allegiance);

	// The death path's two mechanisms: DeathSpawn's payload (asteroid/
	// rubble) and SweepsOwnedOnDeath's generic sweep (ordnance) --
	// mutually exclusive per entity, run from both death call sites.
	void runDeathResponses(EntityId id) noexcept;

	// Destroys the entity and updates count_.
	void removeElement(EntityId id) noexcept;

	// Re-sorts the Order pool by (layer, seq) iff a spawn or destroy has
	// touched it since the last sort -- make() and removeElement set
	// orderDirty_, so a steady-state frame sorts nothing.
	void ensureOrdered();

	// The declared position every spawn gets: the caller's layer, FIFO
	// within it. One counter, one place it advances, so make() and the
	// spawn() family cannot drift apart on what seq means.
	[[nodiscard]] Order nextOrder(Layer layer) noexcept;

	entt::registry reg_;
	// Set true by anything that adds or removes an Order -- see
	// ensureOrdered(). Starts true: an empty pool has nothing to sort, but
	// false would risk skipping the first real one.
	bool orderDirty_ = true;
	usize count_ = 0;
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

	// The command buffer: filled in pipeline order by queueSpawn, drained
	// in that same order at the sync point.
	std::vector<SpawnCommand> spawnCommands_;

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
			b_.attach<U>(id_);
		else
			b_.attach<U>(id_, std::forward<T>(value));
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
