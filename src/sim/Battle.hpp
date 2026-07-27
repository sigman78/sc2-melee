// Copyright the Ur-Quan Masters contributors. GPL-2.0-or-later.

#ifndef UQM2_SIM_BATTLE_HPP
#define UQM2_SIM_BATTLE_HPP

#include "sim/Element.hpp"
#include "sim/EntityList.hpp"
#include "sim/Random.hpp"
#include "sim/Ship.hpp"

#include <cstdint>
#include <span>
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
	EntityId a;
	EntityId b;

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
	EntityId id;
	ElementKind kind = ElementKind::Unknown;
	std::int32_t playerNr = -1;
};

class Battle
{
public:
	explicit Battle(std::uint32_t seed) : rng_(seed) {}

	[[nodiscard]] Rng &rng() noexcept { return rng_; }
	[[nodiscard]] EntityList<Element> &elements() noexcept { return elements_; }
	[[nodiscard]] const EntityList<Element> &elements() const noexcept
	{
		return elements_;
	}

	// Returns a checked reference, not a raw pointer -- see EntityRef in
	// EntityList.hpp. `auto e = b.get(id)` works; `Element *e = b.get(id)`
	// deliberately does not.
	[[nodiscard]] auto get(EntityId id) noexcept { return elements_.get(id); }
	[[nodiscard]] auto get(EntityId id) const noexcept
	{
		return elements_.get(id);
	}

	// Adds an element at the head or the tail: order is gameplay (design-notes
	// D8). The Pkunk's phoenix is head-inserted so it preprocesses before the
	// dead Pkunk's death hook runs (pkunk.c:498-512), which is the reincarnation.
	EntityId spawnFront(Element e);
	EntityId spawnBack(Element e);

	// One simulation step, 1/24 second of game time.
	void step();

	[[nodiscard]] std::uint64_t frame() const noexcept { return frame_; }

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

	// Components: per-kind state keyed by the entity, beside the battle
	// rather than inside Element (review-002 §1). Storage is a flat vector
	// with linear find -- a melee holds a handful of each, and the stable
	// generation-checked id keeps the ordered-list spine untouched.

	// The ship component. Null for anything that is not a ship.
	[[nodiscard]] ShipState *ship(EntityId id) noexcept;
	[[nodiscard]] const ShipState *ship(EntityId id) const noexcept;
	ShipState &attachShip(EntityId id, Borrowed<const ShipSpec> spec);

	// The weapon-guidance component: which WeaponSpec a shot flies by --
	// ends the old abuse of ShipState as a spec-pointer carrier on weapons.
	[[nodiscard]] Borrowed<const WeaponSpec> weaponSpec(
			EntityId id) const noexcept;
	void attachWeaponSpec(EntityId id, Borrowed<const WeaponSpec> spec);

private:
	void preProcessPass();
	void postProcessPass();
	void catchUpFrom(EntityId first);
	void preProcessOne(EntityId id) noexcept;

	// ProcessCollisions (process.c:361-627): walks candidates from `first`,
	// preprocessing stragglers via processedMask (process_flags) -- see
	// design-notes D1. Returns whether `elem` ended the walk stopped.
	bool processCollisions(EntityId elem, EntityId first, TimeValue maxTime,
			ElementFlags processedMask);
	bool resolveAgainst(EntityId elem, EntityId test, EntityId succ,
			TimeValue maxTime, ElementFlags processedMask);
	void killOverlapSpawn(EntityId id);
	void recordSpawn(EntityId id, const Element &e);
	void dropComponents(EntityId id) noexcept;

	EntityList<Element> elements_;
	Rng rng_;
	std::uint64_t frame_ = 0;

	// Both reused across steps so a steady-state frame allocates nothing.
	std::vector<CollisionEvent> collisions_;
	std::vector<SpawnEvent> spawns_;

	// Component sidecars, erased when their entity is reaped.
	std::vector<std::pair<EntityId, ShipState>> ships_;
	std::vector<std::pair<EntityId, Borrowed<const WeaponSpec>>> weaponSpecs_;
};

}  // namespace uqm::sim

#endif  // UQM2_SIM_BATTLE_HPP
