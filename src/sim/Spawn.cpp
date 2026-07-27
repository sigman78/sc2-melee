// Copyright the Ur-Quan Masters contributors. GPL-2.0-or-later.

#include "Spawn.hpp"

#include "sim/Trig.hpp"
#include "sim/World.hpp"

#include <cassert>
#include <cstddef>

namespace uqm::sim {

Vec2i
muzzlePosition(const ShipView &ship) noexcept
{
	// The C spells this out per ship as `pixoffs` fed through
	// initialize_missile, which places the projectile that many *display*
	// pixels along the facing from the ship's next position.
	const int angle = facingToAngle(normalizeFacing(ship.facing));
	const std::int32_t offset = displayToWorld(ship.muzzleOffset);
	return Vec2i{ship.position.x + cosine(angle, offset),
		ship.position.y + sine(angle, offset)};
}

std::size_t
spawnCruiserPrimary(const ShipView &ship, std::span<Spawn> out) noexcept
{
	assert(!out.empty() && "spawn buffer must have room");

	Spawn &s = out[0];
	s.position = muzzlePosition(ship);
	s.facing = normalizeFacing(ship.facing);
	s.speed = ship.weaponSpeed;
	s.life = ship.weaponLife;
	s.damage = ship.weaponDamage;
	s.hitPoints = ship.weaponHitPoints;
	s.blastOffset = ship.blastOffset;
	s.playerNr = ship.playerNr;

	// human.c:281 -- `face = index = ShipFacing`. The nuke sprite is
	// directional, so its frame follows the facing.
	s.frameIndex = static_cast<std::uint16_t>(s.facing);

	// human.c:283 -- flags = 0. Cruiser missiles collide with each other.
	s.ignoreSimilar = false;
	return 1;
}

std::size_t
spawnAvengerPrimary(const ShipView &ship, std::span<Spawn> out) noexcept
{
	assert(!out.empty() && "spawn buffer must have room");

	Spawn &s = out[0];
	s.position = muzzlePosition(ship);
	s.facing = normalizeFacing(ship.facing);
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

}  // namespace uqm::sim
