// Copyright the Ur-Quan Masters contributors. GPL-2.0-or-later.

#ifndef UQM2_SIM_BATTLE_HPP
#define UQM2_SIM_BATTLE_HPP

#include "sim/Element.hpp"
#include "sim/Entity.hpp"
#include "sim/Random.hpp"
#include "sim/Ship.hpp"

#include <entt/entity/registry.hpp>

#include <cstddef>
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
	std::int32_t playerNr = -1;
};

class Battle
{
public:
	explicit Battle(std::uint32_t seed);

	[[nodiscard]] Rng &rng() noexcept { return rng_; }

	// The ordered walk, ex-EntityList: the registry stores, the OrderLink
	// spine orders (Entity.hpp). front/next is the walk every ordered pass
	// makes; alive() is the generation check a stale handle fails.
	[[nodiscard]] EntityId front() const noexcept { return head_; }
	[[nodiscard]] EntityId back() const noexcept { return tail_; }
	[[nodiscard]] EntityId next(EntityId id) const noexcept;
	[[nodiscard]] EntityId prev(EntityId id) const noexcept;
	[[nodiscard]] bool alive(EntityId id) const noexcept
	{
		return reg_.valid(id);
	}
	[[nodiscard]] std::size_t size() const noexcept { return count_; }

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

	// Adds an element at the head or the tail: order is gameplay (design-notes
	// D8). The Pkunk's phoenix is head-inserted so it preprocesses before the
	// dead Pkunk's death hook runs (pkunk.c:498-512), which is the reincarnation.
	// insertAfter is the C's InsertElement, kept for the ships that splice.
	EntityId spawnFront(Element e);
	EntityId spawnBack(Element e);
	EntityId insertAfter(EntityId after, Element e);

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

	// The world itself, for component types the sim does not know --
	// presentation attaches its own (review-004 open question 3: ownership
	// is by component type, not by store; sim/ never names an app type).
	[[nodiscard]] entt::registry &registry() noexcept { return reg_; }
	[[nodiscard]] const entt::registry &registry() const noexcept
	{
		return reg_;
	}

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

	// The spine ops, ex-EntityList::linkAfter/remove.
	EntityId spawn(EntityId after, Element e);
	void linkAfter(EntityId after, EntityId id) noexcept;
	void removeElement(EntityId id) noexcept;

	entt::registry reg_;
	EntityId head_ = kNoEntity;
	EntityId tail_ = kNoEntity;
	std::size_t count_ = 0;
	Rng rng_;
	std::uint64_t frame_ = 0;

	// Both reused across steps so a steady-state frame allocates nothing.
	std::vector<CollisionEvent> collisions_;
	std::vector<SpawnEvent> spawns_;
};

}  // namespace uqm::sim

#endif  // UQM2_SIM_BATTLE_HPP
