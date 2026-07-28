// Copyright the Ur-Quan Masters contributors. GPL-2.0-or-later.

#ifndef UQM2_SIM_SHIPS_ILWRATH_HPP
#define UQM2_SIM_SHIPS_ILWRATH_HPP

#include "sim/Ship.hpp"
#include "sim/Spawn.hpp"

namespace uqm::sim {

class Battle;

// Ilwrath Avenger: one forward flame.
[[nodiscard]] usize spawnAvengerPrimary(
		const ShipView &ship, std::span<Spawn> out) noexcept;

// The Avenger's flame: the animation is the projectile -- its frame (and
// collision silhouette) grows every frame it lives (ilwrath.c:126-139), now
// WeaponSpec::frameDriven and the animate pass's own sub-iteration
// (Battle.cpp). Its impact lingering one frame instead of vanishing
// (ilwrath.c:141-148) is Warhead::lingersOnHit, a data bit weaponCollision
// reads itself.

// The Avenger's ship hook: the whole cloak state machine, activation
// included (ilwrath_preprocess, ilwrath.c:232-394). Runs in the pre phase
// because the C's does -- see ShipSpec::preProcess.
void ilwrathPreProcess(Battle &b, EntityId id) noexcept;

[[nodiscard]] const ShipSpec &ilwrathAvenger() noexcept;

}  // namespace uqm::sim

#endif  // UQM2_SIM_SHIPS_ILWRATH_HPP
