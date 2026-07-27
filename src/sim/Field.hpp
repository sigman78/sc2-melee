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
// unbounded number of RNG draws -- two per attempt. Call it before the
// asteroids, as init.c does, or the stream diverges.
EntityId spawnPlanet(Battle &b, const CollisionMask *mask);

// Places one asteroid on an arena edge (misc.c:131-201).
//
// Consumes exactly six RNG draws, in this order: the edge selector, the
// position along that edge, the speed, the heading, the starting sprite
// rotation, and the spin rate. The C spells the order out in a comment
// (misc.c:176-178) because argument evaluation order was desynchronising
// network games, so it is reproduced literally.
EntityId spawnAsteroid(Battle &b, const CollisionMask *mask);

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
