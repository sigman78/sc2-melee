// Copyright the Ur-Quan Masters contributors. GPL-2.0-or-later.

#include "Ilwrath.hpp"

#include "engine/core/Types.hpp"
#include "sim/Battle.hpp"
#include "sim/Damage.hpp"
#include "sim/ShipSystems.hpp"
#include "sim/Targeting.hpp"
#include "sim/Trig.hpp"
#include "sim/Velocity.hpp"

#include <cassert>

namespace uqm::sim {

usize spawnAvengerPrimary(const ShipView &ship, std::span<Spawn> out) noexcept
{
	assert(!out.empty() && "spawn buffer must have room");

	Spawn &s = out[0];
	s.position = muzzlePosition(ship);
	s.facing = ship.facing;
	s.speed = ship.weaponSpeed;
	s.life = ship.weaponLife;
	s.damage = ship.weaponDamage;
	s.hitPoints = ship.weaponHitPoints;
	s.blastOffset = ship.blastOffset;
	s.playerNr = ship.playerNr;

	// ilwrath.c:200-201 -- `face = ShipFacing` but `index = 0`. The flame is
	// not a directional sprite, so unlike the Cruiser its frame does not
	// track the facing. Easy to "tidy" into symmetry and wrong if you do.
	s.frameIndex = 0;

	// ilwrath.c:203 -- IGNORE_SIMILAR. A stream of flame must not collide
	// with itself.
	s.ignoreSimilar = true;

	// ilwrath.c:219-222 -- the flame rides the Avenger's own velocity rather
	// than leaving it behind, so the stream trails the ship instead of hanging
	// in the space it just left.
	s.inheritsVelocity = true;
	return 1;
}

const ShipSpec &ilwrathAvenger() noexcept
{
	// ilwrath.c:27-53. THRUST_WAIT 0 and WEAPON_WAIT 0: the Avenger
	// accelerates every frame and its flame is continuous.
	static const ShipSpec data{
			.maxCrew = 22,
			.thrust{.max = 25, .increment = 5},
			.battery{.max = 16, .regen = 4, .wait = 4},
			.thrustWait = 0,
			.turnWait = 2,
			.mass = 7,
			.weapon{
					.wait = 0,
					.energyCost = 1,
					.speed = 25,         // MISSILE_SPEED == MAX_THRUST
					.muzzleOffset = 29,  // ILWRATH_OFFSET
					.lifetime{.remaining = 8},
					.vitality{.hitPoints = 1},
					.warhead{.damage = 1,
							.blastOffset = 0,
							.lingersOnHit = true},  // MISSILE_OFFSET
					.spawn = spawnAvengerPrimary,
					.frameDriven = true,
			},
			.special{
					.wait = 13,
					.energyCost = 3,
					// The cloak runs in the pre-turn slot, winning the energy
					// race against the same frame's shot (Specials.hpp).
			},
			.equip =
					[](Battle &b, EntityId id) noexcept {
						b.reg.emplace<comp::Cloak>(id);
					},
	};
	return data;
}

}  // namespace uqm::sim
