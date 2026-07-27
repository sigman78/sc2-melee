// Copyright the Ur-Quan Masters contributors. GPL-2.0-or-later.

#ifndef UQM2_SIM_TARGETING_HPP
#define UQM2_SIM_TARGETING_HPP

#include "sim/Entity.hpp"
#include "sim/Trig.hpp"

namespace uqm::sim {

class Battle;

// TrackShip (weapon.c:319-412): steers `facing` toward the nearest living
// enemy ship (by player, not owner). Returns -1 for no target, else the
// facing delta -- load-bearing: cloak auto-aim tests `>= 0`, nuke ignores it.
[[nodiscard]] int trackShip(Battle &b, EntityId tracker, Facing &facing,
		EntityId *outTarget = nullptr) noexcept;

}  // namespace uqm::sim

#endif  // UQM2_SIM_TARGETING_HPP
