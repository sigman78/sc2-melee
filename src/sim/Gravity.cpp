// Copyright the Ur-Quan Masters contributors. GPL-2.0-or-later.

#include "Gravity.hpp"

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

	// Which endpoint to measure from: the C decides once, from self's
	// PRE_PROCESS flag, applied to both sides of every pair (gravity.c:50-63)
	// -- reproduced as-is, since it decides which frame the pull lands on.
	const bool useNext = any(self->flags & ElementFlags::PreProcessed);
	const Vec2i from = useNext ? self->next : self->current;

	const std::int32_t pull = worldToVelocity(1);

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

		const Vec2i to = useNext ? t->next : t->current;
		const Vec2i d = wrapDelta(Vec2i{from.x - to.x, from.y - to.y});

		// The disc is measured in display pixels, and the cheap per-axis
		// rejection comes first exactly as in the C.
		const std::int32_t adx = worldToDisplay(d.x < 0 ? -d.x : d.x);
		const std::int32_t ady = worldToDisplay(d.y < 0 ? -d.y : d.y);
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
planetPostProcess(Battle &b, EntityId id) noexcept
{
	(void)calculateGravity(b, id);
}

}  // namespace uqm::sim
