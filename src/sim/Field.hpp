// Copyright the Ur-Quan Masters contributors. GPL-2.0-or-later.

#ifndef UQM2_SIM_FIELD_HPP
#define UQM2_SIM_FIELD_HPP

#include "sim/EntityList.hpp"

#include <cstdint>

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

// Places the planet (misc.c:40-76).
//
// Position is rejected and redrawn while it would either sit in another
// element's gravity well or physically overlap something, so this consumes an
// unbounded number of RNG draws -- two per attempt. init.c:228-233 spawns the
// five asteroids FIRST and the planet second; keep that order or the stream
// diverges. (An earlier version of this comment had it backwards.)
EntityId spawnPlanet(Battle &b, const CollisionMask *mask);

// Places one asteroid on an arena edge (misc.c:131-201).
//
// Consumes exactly seven RNG draws, in this order: the edge selector, the
// position along that edge, the speed, the heading, the starting sprite
// rotation, the spin rate, and the spin direction (bit 7 into thrust_wait,
// misc.c:192-193). The C spells the order out in a comment (misc.c:176-178)
// because argument evaluation order was desynchronising network games, so it
// is reproduced literally.
EntityId spawnAsteroid(Battle &b, const CollisionMask *mask);

// Drop an already-spawned ship somewhere random and legal.
//
// The C's rule (ship.c:473-481) is a rejection loop: any display-aligned spot
// in the arena, retried while it sits in a gravity well or overlaps existing
// matter. The facing is random too (ship.c:456) -- and because the collision
// mask is per-facing, the caller must have set the facing and its mask before
// calling, or the loop tests the wrong silhouette.
//
// `minSeparation` is an addition, not the C. The C will happily drop two ships
// next to each other, which in a two-ship melee means the battle opens with
// them already on top of one another. It is a floor in world units, measured
// across the torus, and it is relaxed if enough attempts fail so that a
// crowded arena can still place everyone.
void placeShipAtRandom(Battle &b, EntityId ship, std::int32_t minSeparation);

// The rubble an asteroid leaves, whose own death spawns a replacement
// asteroid -- which is what keeps the field's population constant
// (misc.c:80-105). Nothing outside the field needs to call this; it is the
// asteroid's death hook.
void asteroidDeath(Battle &b, EntityId id) noexcept;

// Whether `id` physically overlaps any other collidable element, or any
// player ship, where it currently stands (TimeSpaceMatterConflict,
// gravity.c:151-199). Used only for placement.
[[nodiscard]] bool timeSpaceMatterConflict(Battle &b, EntityId id);

}  // namespace uqm::sim

#endif  // UQM2_SIM_FIELD_HPP
