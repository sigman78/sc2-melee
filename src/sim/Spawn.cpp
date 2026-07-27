// Copyright the Ur-Quan Masters contributors. GPL-2.0-or-later.

#include "Spawn.hpp"

#include "engine/core/Types.hpp"
#include "sim/Trig.hpp"
#include "sim/World.hpp"

namespace uqm::sim {

Vec2i
muzzlePosition(const ShipView &ship) noexcept
{
	// The C spells this out per ship as `pixoffs` fed through
	// initialize_missile, which places the projectile that many *display*
	// pixels along the facing from the ship's next position.
	const Angle angle = ship.facing.angle();
	const i32 offset = displayToWorld(ship.muzzleOffset);
	return Vec2i{ship.position.x + cosine(angle, offset),
		ship.position.y + sine(angle, offset)};
}

}  // namespace uqm::sim
