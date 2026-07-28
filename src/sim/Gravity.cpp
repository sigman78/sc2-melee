// Copyright the Ur-Quan Masters contributors. GPL-2.0-or-later.

#include "Gravity.hpp"

#include "engine/core/Types.hpp"
#include "sim/Battle.hpp"
#include "sim/Trig.hpp"
#include "sim/World.hpp"

namespace uqm::sim {

bool
calculateGravity(Battle &b, EntityId id)
{
	auto self = b.get(id);
	if (self == nullptr)
		return false;

	const bool selfHasGravity =
			self->collidable() && isGravitySource(self->mass);

	// Doc §2 refinement 1: gravity now runs as its own pipeline pass right
	// after GuidedSteer, before Integrate has touched anyone's `next` this
	// frame -- `current` is the one consistent snapshot every entity shares
	// at that point, so there is no more "which half of the walk already
	// moved" flag to consult.
	const Vec2i from = self->current;

	const i32 pull = worldToVelocity(1);

	for (EntityId other = b.front(); other != kNoEntity;
			other = b.next(other))
	{
		if (other == id)
			continue;

		auto t = b.get(other);
		if (t == nullptr || !t->collidable())
			continue;

		// Only pairs that disagree about being a source are interesting: two
		// planets ignore each other, and so do two ordinary elements.
		const bool testHasGravity = isGravitySource(t->mass);
		if (testHasGravity == selfHasGravity)
			continue;

		const Vec2i to = t->current;
		const Vec2i d = wrapDelta(Vec2i{from.x - to.x, from.y - to.y});

		// The disc is measured in display pixels, and the cheap per-axis
		// rejection comes first exactly as in the C.
		const i32 adx = worldToDisplay(d.x < 0 ? -d.x : d.x);
		const i32 ady = worldToDisplay(d.y < 0 ? -d.y : d.y);
		if (adx > kGravityRadius || ady > kGravityRadius)
			continue;
		if (adx * adx + ady * ady > kGravityRadius * kGravityRadius)
			continue;

		if (testHasGravity)
		{
			// We are the light one, and we are inside their well. The C
			// breaks out here without touching anything.
			return true;
		}

		// `d` points from the test element toward us, so this accelerates it
		// inward -- one world unit per frame, no falloff.
		const int angle = arctan(d.x, d.y);
		t->velocity.deltaComponents(cosine(angle, pull), sine(angle, pull));

		if (b.has<PlayerShip>(other))
		{
			// gravity.c:136-137 clears SHIP_AT_MAX_SPEED but deliberately
			// leaves SHIP_BEYOND_MAX_SPEED alone: a ship already whipped past
			// its maximum stays flagged as such.
			if (ShipState *ss = b.ship(other))
			{
				if (ss->speed == SpeedState::AtMax)
					ss->speed = SpeedState::Normal;
				ss->inGravityWell = true;
			}
		}
	}

	return false;
}

void
gravityPass(Battle &b)
{
	for (EntityId id = b.front(); id != kNoEntity; id = b.next(id))
	{
		const Element *e = b.get(id);
		if (e == nullptr || !e->collidable() || !isGravitySource(e->mass))
			continue;
		(void)calculateGravity(b, id);
	}
}

}  // namespace uqm::sim
