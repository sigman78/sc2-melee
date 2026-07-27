// Copyright the Ur-Quan Masters contributors. GPL-2.0-or-later.

#ifndef UQM2_SIM_BATTLE_HPP
#define UQM2_SIM_BATTLE_HPP

#include "sim/Element.hpp"
#include "sim/EntityList.hpp"
#include "sim/Random.hpp"

#include <cstdint>
#include <span>
#include <vector>

namespace uqm::sim {

// One battle's simulation state, and its step.
//
// This is RedrawQueue (process.c:1013) with the drawing taken out. The name is
// a lie in the C: PreProcessQueue and PostProcessQueue *are* the simulation,
// shared by melee, hyperspace (hyper.c:1368), interplanetary (ipdisp.c:567)
// and -- via a fork -- the lander. Building it for melee is what buys
// hyperspace's loop in M3.
//
// Deterministic and self-contained: no wall clock, no I/O, no globals. The
// RNG is a member, so a battle replays from its seed and presentation cannot
// perturb it by drawing from the same stream (commanim.c currently does).
// What happened when two things touched.
//
// Recorded rather than merely acted on, because more than one consumer needs
// it and none of them belong in the collision code: the debug overlay draws
// contact points and response vectors, and the sound layer needs to know an
// impact happened and how hard. Observational only -- nothing here feeds back
// into the step, so recording it cannot change the simulation.
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

	// Adds an element at the head or the tail. Head insertion matters: the
	// Pkunk's phoenix is head-inserted so it preprocesses *before* the dead
	// Pkunk's death hook runs (pkunk.c:498-512), and that ordering is the
	// reincarnation.
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

private:
	void preProcessPass();
	void postProcessPass();
	void preProcessOne(EntityId id) noexcept;
	void collideAgainstSuccessors(EntityId id);
	void collideAgainstAll(EntityId id);
	bool testPair(EntityId a, EntityId b);

	EntityList<Element> elements_;
	Rng rng_;
	std::uint64_t frame_ = 0;

	// Both reused across steps so a steady-state frame allocates nothing.
	std::vector<EntityId> scratch_;
	std::vector<CollisionEvent> collisions_;
};

}  // namespace uqm::sim

#endif  // UQM2_SIM_BATTLE_HPP
