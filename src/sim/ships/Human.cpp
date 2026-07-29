// Copyright the Ur-Quan Masters contributors. GPL-2.0-or-later.

#include "Human.hpp"

#include "engine/core/Types.hpp"
#include "sim/Battle.hpp"
#include "sim/Damage.hpp"
#include "sim/ShipSystems.hpp"
#include "sim/World.hpp"

#include <cassert>
#include <utility>

namespace uqm::sim {

usize spawnCruiserPrimary(const ShipView &ship, std::span<Spawn> out) noexcept
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

	// human.c:281 -- `face = index = ShipFacing`. The nuke sprite is
	// directional, so its frame follows the facing.
	s.frameIndex = static_cast<u16>(s.facing.raw());

	// human.c:283 -- flags = 0. Cruiser missiles collide with each other.
	s.ignoreSimilar = false;
	return 1;
}

const ShipSpec &earthlingCruiser() noexcept
{
	// human.c:26-55. Speeds store post-DISPLAY_TO_WORLD values; offsets
	// store raw display pixels.
	static const ShipSpec data{
			.maxCrew = 18,
			.thrust{.max = 24, .increment = 3},
			.battery{.max = 18, .regen = 1, .wait = 8},
			.thrustWait = 4,
			.turnWait = 1,
			.mass = 6,
			.weapon{
					.wait = 10,
					.energyCost = 9,
					.speed = 40,  // max(MAX_THRUST, DISPLAY_TO_WORLD(10)),
								  // human.c:42-45
					.muzzleOffset = 42,  // HUMAN_OFFSET
					.lifetime{.remaining = 60},
					.vitality{.hitPoints = 1},
					.warhead{.damage = 4, .blastOffset = 8},  // NUKE_OFFSET
					// Guided and accelerating (human.c:43-50): TRACK_WAIT 3,
					// DISPLAY_TO_WORLD(20) == 80, DISPLAY_TO_WORLD(1) == 4. The
					// clock starts already wound to trackWait
					// (human.c:297-299).
					.guided = comp::Guided{.trackWait = 3,
							.maxSpeed = 80,
							.thrustScale = 4,
							.clock = 3},
					.spawn = spawnCruiserPrimary,
			},
			.special{
					.wait = 9,
					.energyCost = 4,
			},
			.equip =
					[](Battle &b, EntityId id) noexcept {
						// LASER_RANGE (human.c:55), in display pixels.
						b.reg.emplace<comp::PointDefence>(
								id, comp::PointDefence{100});
					},
	};
	return data;
}

}  // namespace uqm::sim
