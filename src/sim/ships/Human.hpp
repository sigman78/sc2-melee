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

// The Cruiser equips PointDefence; the laser that burns shots in range is a
// mechanic, not this ship's hook (Specials.hpp).

const ShipSpec &earthlingCruiser() noexcept;

}  // namespace uqm::sim

#endif  // UQM2_SIM_SHIPS_HUMAN_HPP
