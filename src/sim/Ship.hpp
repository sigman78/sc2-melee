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

	// Guided-weapon parameters (human.c:36-44). Zero for a weapon that just
	// flies straight, which is most of them.
	std::int32_t weaponTrackWait = 0;
	std::int32_t weaponMaxSpeed = 0;
	std::int32_t weaponThrustScale = 0;

	// What the shot does each frame, if anything.
	ElementHook weaponPreProcess = nullptr;

	// What SPECIAL does. The engine only ticks special_counter down
	// (ship.c:342-343); everything a special actually *does* is per-ship, and
	// that asymmetry is most of why ships/ is 25 files.
	ElementHook special = nullptr;

	// Point defence: how far it reaches, in display pixels (LASER_RANGE,
	// human.c:55). Zero for a ship without one.
	std::int32_t pointDefenceRange = 0;

	// One mask per facing, in facing order. A ship's silhouette changes as it
	// turns -- that is the whole reason there are sixteen cels -- so the mask
	// has to follow the facing or collision is tested against whichever
	// rotation happened to be current at spawn. Empty until content is loaded;
	// sim/ never fills this in, because sim/ does not read files.
	std::span<const CollisionMask> facingMasks;

	// A descriptor nobody filled in. Every field defaults to zero, which is
	// not a slow ship -- it is a ship that cannot accelerate at all, turns
	// every frame, and has no crew. That reads as a control bug rather than as
	// missing data, so it is worth being able to ask.
	[[nodiscard]] constexpr bool
	valid() const noexcept
	{
		return maxCrew > 0 && thrust.maxThrust > 0;
	}
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
[[nodiscard]] // TrackShip (weapon.c:319-380): steers `facing` one step toward the nearest
// living enemy ship, and reports whether it moved. Used by any guided weapon.
//
// "Enemy" is by player, not by owner: a missile chases the other side's ship,
// not merely one that is not its own.
[[nodiscard]] int trackShip(Battle &b, EntityId tracker, int &facing) noexcept;

// The Cruiser's nuke, which is guided and accelerates as it flies
// (human.c:128-158).
void nukePreProcess(Battle &b, EntityId id) noexcept;

// The Cruiser's point-defence laser (human.c:161-260): burns down every enemy
// shot in range, paying once for the volley.
void cruiserSpecial(Battle &b, EntityId id) noexcept;

// The Avenger's cloak (ilwrath.c:377-393).
void avengerSpecial(Battle &b, EntityId id) noexcept;

const ShipData &earthlingCruiser() noexcept;
[[nodiscard]] const ShipData &ilwrathAvenger() noexcept;

}  // namespace uqm::sim

#endif  // UQM2_SIM_SHIP_HPP
