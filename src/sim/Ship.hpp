// Copyright the Ur-Quan Masters contributors. GPL-2.0-or-later.

#ifndef UQM2_SIM_SHIP_HPP
#define UQM2_SIM_SHIP_HPP

#include "sim/Element.hpp"
#include "sim/Spawn.hpp"
#include "sim/Thrust.hpp"

#include <cstdint>

namespace uqm::sim {

class Battle;

// An immutable description of a ship type.
//
// A *value*, and constructible in code rather than only parsed. That is not
// theoretical: sis_ship.c:881-990 computes the flagship's whole descriptor
// from inventory, and shofixti.c:461-517 builds a damaged variant by copying
// the descriptor, swapping the art, nulling the glory-device sprites and
// setting weapon_wait = 10. So a loaded file is one way to make one and a
// builder function is another.
struct ShipData
{
	std::int32_t maxCrew = 0;
	std::int32_t maxEnergy = 0;
	std::int32_t energyRegen = 0;
	std::int32_t energyWait = 0;

	ThrustProfile thrust;
	std::int32_t thrustWait = 0;
	std::int32_t turnWait = 0;

	std::int32_t weaponWait = 0;
	std::int32_t weaponEnergyCost = 0;
	std::int32_t specialWait = 0;
	std::int32_t specialEnergyCost = 0;

	std::int32_t mass = 0;

	// The primary weapon, as a pure descriptor function (Spawn.hpp).
	SpawnFn spawnPrimary = nullptr;

	// The weapon's own parameters, handed to the spawn function.
	std::int32_t weaponSpeed = 0;
	std::int32_t weaponLife = 0;
	std::int32_t weaponDamage = 0;
	std::int32_t weaponHitPoints = 0;
	std::int32_t muzzleOffset = 0;
	std::int32_t blastOffset = 0;

	const CollisionMask *hullMask = nullptr;
	// One per facing, like facingMasks below. A missile is drawn from the cel
	// matching the direction it flies, so it has to collide as that cel --
	// indexing this by 0 and drawing by facing is what makes a rocket appear
	// squashed into the wrong box.
	std::span<const CollisionMask> weaponMasks;

	// One mask per facing, in facing order. A ship's silhouette changes as it
	// turns -- that is the whole reason there are sixteen cels -- so the mask
	// has to follow the facing or collision is tested against whichever
	// rotation happened to be current at spawn. Empty until content is loaded;
	// sim/ never fills this in, because sim/ does not read files.
	std::span<const CollisionMask> facingMasks;
};

// The two halves of a ship's frame, matching ship.c:149-280 and 282-347.
//
// The split is not arbitrary: turning and thrusting happen in the pre pass so
// the frame's motion includes them, and firing happens in the post pass so a
// weapon spawned this frame is caught up by the step loop rather than moving
// before its owner has.
void shipPreProcess(Battle &b, EntityId id) noexcept;
void shipPostProcess(Battle &b, EntityId id) noexcept;

// The two M1 ships, from human.c and ilwrath.c.
[[nodiscard]] const ShipData &earthlingCruiser() noexcept;
[[nodiscard]] const ShipData &ilwrathAvenger() noexcept;

}  // namespace uqm::sim

#endif  // UQM2_SIM_SHIP_HPP
