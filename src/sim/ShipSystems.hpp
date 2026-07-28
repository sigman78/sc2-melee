// Copyright the Ur-Quan Masters contributors. GPL-2.0-or-later.

#ifndef UQM2_SIM_SHIPSYSTEMS_HPP
#define UQM2_SIM_SHIPSYSTEMS_HPP

#include "engine/core/Types.hpp"
#include "sim/Element.hpp"
#include "sim/Ship.hpp"

namespace uqm::sim {

class Battle;

// Energy regeneration (ship.c:225-230), as a whole-battle pass run before
// shipPreProcess so every consumer reads its own ship's post-regen energy.
void energyRegenPass(Battle &b) noexcept;

// The two halves of a ship's frame (ship.c:149-280, 282-347): turning and
// thrusting in the pre pass, firing in the post pass so a spawned weapon is
// caught up by the step loop this frame -- see design-notes D1.
void shipPreProcess(Battle &b, EntityId id) noexcept;
void shipPostProcess(Battle &b, EntityId id) noexcept;

// One point of exhaust, dropped behind a thrusting ship (tactrans.c:792-840).
void spawnIonTrail(Battle &b, EntityId ship) noexcept;

// Turns a dead ship into its own explosion rather than removing it
// (StartShipExplosion, tactrans.c:703-728).
void startShipExplosion(Battle &b, EntityId id) noexcept;

// The generic guided-shot step: track, then accelerate (human.c:128-158).
void guidedShotPreProcess(Battle &b, EntityId id) noexcept;

// Shared with the per-ship translation units (ships/Human.cpp,
// ships/Ilwrath.cpp), which spend energy and re-derive the facing mask
// from their own hooks.
bool deltaEnergy(ShipState &s, i32 delta) noexcept;
void applyFacingMask(Element &e, const ShipSpec &spec) noexcept;

}  // namespace uqm::sim

#endif  // UQM2_SIM_SHIPSYSTEMS_HPP
