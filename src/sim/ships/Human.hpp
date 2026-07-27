// Copyright the Ur-Quan Masters contributors. GPL-2.0-or-later.

#ifndef UQM2_SIM_SHIPS_HUMAN_HPP
#define UQM2_SIM_SHIPS_HUMAN_HPP

#include "sim/Ship.hpp"
#include "sim/Spawn.hpp"

namespace uqm::sim {

class Battle;

// Earthling Cruiser: one forward missile.
[[nodiscard]] usize spawnCruiserPrimary(
		const ShipView &ship, std::span<Spawn> out) noexcept;

// The Cruiser's point-defence laser (human.c:161-260): burns down every enemy
// shot in range, paying once for the volley.
void cruiserSpecial(Battle &b, EntityId id) noexcept;

const ShipSpec &earthlingCruiser() noexcept;

}  // namespace uqm::sim

#endif  // UQM2_SIM_SHIPS_HUMAN_HPP
