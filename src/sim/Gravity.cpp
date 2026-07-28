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
	if (!b.alive(id))
		return false;

	const bool selfHasGravity =
			b.collidable(id) && isGravitySource(b.find<Physique>(id)->mass);

	// Doc §2 refinement 1: gravity now runs as its own pipeline pass right
	// after GuidedSteer, before Integrate has touched anyone's `next` this
	// frame -- `current` is the one consistent snapshot every entity shares
	// at that point, so there is no more "which half of the walk already
	// moved" flag to consult.
	const Vec2i from = b.find<Position>(id)->current;

	const i32 pull = worldToVelocity(1);

	bool insideAWell = false;
	b.eachOrdered([&](EntityId other) {
		if (insideAWell || other == id)
			return;

		if (!b.alive(other) || !b.collidable(other))
			return;

		// Only pairs that disagree about being a source are interesting: two
		// planets ignore each other, and so do two ordinary elements.
		const bool testHasGravity =
				isGravitySource(b.find<Physique>(other)->mass);
		if (testHasGravity == selfHasGravity)
			return;

		const Vec2i to = b.find<Position>(other)->current;
		const Vec2i d = wrapDelta(from - to);

		// The disc is measured in display pixels, and the cheap per-axis
		// rejection comes first exactly as in the C.
		const i32 adx = worldToDisplay(d.x < 0 ? -d.x : d.x);
		const i32 ady = worldToDisplay(d.y < 0 ? -d.y : d.y);
		if (adx > kGravityRadius || ady > kGravityRadius)
			return;
		if (adx * adx + ady * ady > kGravityRadius * kGravityRadius)
			return;

		if (testHasGravity)
		{
			// We are the light one, and we are inside their well. The C
			// breaks out here without touching anything.
			insideAWell = true;
			return;
		}

		// `d` points from the test element toward us, so this accelerates it
		// inward -- one world unit per frame, no falloff.
		const int angle = arctan(d.x, d.y);
		b.find<Motion>(other)->velocity.deltaComponents(
				cosine(angle, pull), sine(angle, pull));

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
	});

	return insideAWell;
}

void
gravityPass(Battle &b)
{
	// The well is whichever entity carries Planet, not whichever entity's
	// mass happens to clear kGravityMass (review-007 W6): there is only
	// ever one, so the tag replaces a scan of every element's Physique.
	b.each<Planet>([&b](EntityId id) {
		if (b.collidable(id))
			(void)calculateGravity(b, id);
	});
}

}  // namespace uqm::sim
