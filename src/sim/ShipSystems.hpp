// Copyright the Ur-Quan Masters contributors. GPL-2.0-or-later.

#ifndef UQM2_SIM_SHIPSYSTEMS_HPP
#define UQM2_SIM_SHIPSYSTEMS_HPP

#include "engine/core/Types.hpp"
#include "sim/Element.hpp"
#include "sim/Ship.hpp"

namespace uqm::sim {

class Battle;

// Builds and spawns a player ship element. Component attach order is pool
// insertion order, and bit-exactness depends on it. `warpIn` selects
// whether WarpingIn is attached.
EntityId spawnPlayerShip(Battle &b, const ShipSpec &spec,
		Borrowed<const CollisionMask> mask, Vec2i at, Facing facing,
		i32 playerNr, bool warpIn);

// Energy regeneration (ship.c:225-230), as a whole-battle pass run before
// the other ship passes so every consumer reads its own ship's post-regen
// energy.
void energyRegenPass(Battle &b) noexcept;

// ShipMachines (pipeline slot 4): WarpingIn/Appearing/dead-hull dispatch.
// The pre-turn specials run in their own pass just ahead of this one, and
// the order between them is load-bearing (Specials.hpp).
void shipMachinesPass(Battle &b) noexcept;

// Turn then Thrust (pipeline slot 5, two passes). Same skip list as
// ShipMachines: WarpingIn, Appearing, dead.
void turnPass(Battle &b) noexcept;
void thrustPass(Battle &b) noexcept;

// GuidedSteer (pipeline slot 6): guidedShotPreProcess over every Guided
// entity.
void guidedSteerPass(Battle &b) noexcept;

// Fire/SpecialGate (pipeline slot 10): weapon fire, then the special gate,
// per ship.
void fireAndSpecialGatePass(Battle &b) noexcept;

// One point of exhaust, dropped behind a thrusting ship (tactrans.c:792-840).
void spawnIonTrail(Battle &b, EntityId ship) noexcept;

// Turns a dead ship into its own explosion rather than removing it
// (StartShipExplosion, tactrans.c:703-728).
void startShipExplosion(Battle &b, EntityId id) noexcept;

// cleanup_dead_ship (tactrans.c:307-337): everything the dying ship still
// owns -- in-flight nukes, the flame stream -- goes with it. The death
// path (Battle.cpp) calls this for any SweepsOwnedOnDeath entity.
void sweepDeadShipOrdnance(Battle &b, EntityId id) noexcept;

// The generic guided-shot step: track, then accelerate (human.c:128-158).
void guidedShotPreProcess(Battle &b, EntityId id) noexcept;

// Shared with the per-ship translation units (ships/Human.cpp,
// ships/Ilwrath.cpp), which spend energy and re-derive the facing mask
// from their own hooks.
bool deltaEnergy(comp::ShipState &s, i32 delta) noexcept;
void applyFacingMask(
		Battle &b, EntityId id, Facing facing, const ShipSpec &spec) noexcept;

}  // namespace uqm::sim

#endif  // UQM2_SIM_SHIPSYSTEMS_HPP
