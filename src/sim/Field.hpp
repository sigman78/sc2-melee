// Copyright the Ur-Quan Masters contributors. GPL-2.0-or-later.

#ifndef UQM2_SIM_FIELD_HPP
#define UQM2_SIM_FIELD_HPP

#include "engine/core/Borrowed.hpp"
#include "engine/core/Types.hpp"
#include "sim/Entity.hpp"

namespace uqm::sim {

class Battle;
class CollisionMask;

// The battlefield furniture: the planet and the asteroid field (misc.c).
//
// Kept apart from Spawn.hpp, which is deliberately pure -- these need the
// battle and draw from its RNG, so they cannot be the side-effect-free
// descriptors a weapon spawn is.

// init.c:228.
inline constexpr int kNumAsteroids = 5;

namespace comp::inline space {

// The asteroid's tumble (misc.c:107-128).
struct Spin
{
	bool backwards = false;
	i32 period = 0;
	i32 countdown = 0;
};

}  // namespace comp::inline space

namespace comp::inline life {

// One rock in the field's recycle (misc.c:80-105), which keeps the
// population constant: a solid asteroid dies into rubble, and the rubble
// dies into a fresh asteroid built from the same mask. `mask` is what one
// phase hands the next -- a kill detaches the Collider immediately, a frame
// or more before the death response runs, so the mask cannot be read back
// off that.
struct Asteroid
{
	enum class Phase : u8
	{
		// Solid, spinning, collidable: the asteroid proper.
		Solid,
		// A five-frame blast that never collides, carrying the mask across
		// the gap. It is a timer, not a second mechanic.
		Rubble,
	};

	Borrowed<const CollisionMask> mask = nullptr;
	Phase phase = Phase::Solid;
};

}  // namespace comp::inline life

// Places the planet (misc.c:40-76): rejected/redrawn while in a gravity
// well or overlapping (two RNG draws/attempt). init.c:228-233 spawns
// asteroids first, planet second -- order matters or the RNG stream diverges.
EntityId spawnPlanet(Battle &b, const CollisionMask *mask);

// Places one asteroid on an arena edge (misc.c:131-201). Consumes exactly
// seven RNG draws in order -- edge, position, speed, heading, sprite
// rotation, spin rate, spin direction (misc.c:192-193, order per
// misc.c:176-178).
EntityId spawnAsteroid(Battle &b, const CollisionMask *mask);

// Drop an already-spawned ship somewhere random and legal: rejection loop
// vs. gravity wells/overlaps, random facing -- caller must set facing/mask
// first (ship.c:473-481, ship.c:456). minSeparation floor: design-notes V7.
void placeShipAtRandom(Battle &b, EntityId id, i32 minSeparation);

// Moves a dead rock to its next phase: a solid one leaves rubble carrying
// its mask, and rubble spends that mask on a replacement asteroid. Called
// from the death path (Battle.cpp) for anything holding an Asteroid, which
// is the field and nothing else.
void advanceAsteroidCycle(Battle &b, EntityId id) noexcept;

// Whether `id` physically overlaps any other collidable element, or any
// player ship, where it currently stands (TimeSpaceMatterConflict,
// gravity.c:151-199). Used only for placement.
[[nodiscard]] bool timeSpaceMatterConflict(Battle &b, EntityId id);

}  // namespace uqm::sim

#endif  // UQM2_SIM_FIELD_HPP
