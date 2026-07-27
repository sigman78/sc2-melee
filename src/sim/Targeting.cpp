// Copyright the Ur-Quan Masters contributors. GPL-2.0-or-later.

#include "Targeting.hpp"

#include "engine/core/Types.hpp"
#include "sim/Battle.hpp"
#include "sim/Ship.hpp"
#include "sim/World.hpp"

namespace uqm::sim {

int
trackShip(Battle &b, EntityId tracker, Facing &facing,
		EntityId *outTarget) noexcept
{
	const auto self = b.get(tracker);
	if (self == nullptr)
		return -1;

	// The C reads `next` once the tracker has been preprocessed and `current`
	// before it (weapon.c:356-368), which is the same distinction gravity.c
	// makes -- and for the same reason: half the list has already moved.
	const bool useNext = any(self->flags & ElementFlags::PreProcessed);
	const Vec2i from = useNext ? self->next : self->current;

	int bestDelta = 0;
	i32 bestDistance = 0;
	EntityId bestTarget;
	bool found = false;

	for (EntityId id = b.front(); id != kNoEntity;
			id = b.next(id))
	{
		const auto t = b.get(id);
		if (t == nullptr || !b.has<PlayerShip>(id))
			continue;
		if (t->playerNr == self->playerNr)
			continue;
		// Dead ships are not targets (weapon.c:352-353).
		const ShipState *ts = b.ship(id);
		if (t->lifeSpan == 0 || ts == nullptr || ts->crew == 0)
			continue;
		// Nor cloaked ones (weapon.c:344-348). This is the whole tactical
		// point of the Ilwrath cloak: not that it is hard to see, but that a
		// guided weapon has nothing to steer toward.
		if (isCloaked(b, id))
			continue;

		const Vec2i to = useNext ? t->next : t->current;
		const Vec2i d = wrapDelta(Vec2i{to.x - from.x, to.y - from.y});
		const int deltaFacing = Angle(arctan(d.x, d.y)).facing() - facing;

		// Nearest target, by |dx| + |dy| -- the C's own stated approximation of
		// the real distance (weapon.c:378-385).
		const i32 adx = d.x < 0 ? -d.x : d.x;
		const i32 ady = d.y < 0 ? -d.y : d.y;
		const i32 distance = adx + ady;

		if (!found || distance < bestDistance)
		{
			bestDistance = distance;
			bestDelta = deltaFacing;
			bestTarget = id;
			found = true;
		}
	}

	// The C's return is best_delta_facing: -1 when nothing was targetable,
	// 0 when dead ahead (weapon.c:410-412). The nuke never looks, but the
	// cloak's auto-aim gates on `>= 0`.
	if (!found)
		return -1;
	if (outTarget != nullptr)
		*outTarget = bestTarget;
	if (bestDelta == 0)
		return 0;

	// weapon.c:398-410: one step per call, direction by which side of a
	// half-circle the target sits -- a coin flip exactly astern, since always
	// choosing the same side would make a missile behind you predictable.
	int turn = 0;
	if (bestDelta == kNumFacings / 2)
		turn = static_cast<int>((b.rng().next() & 1u) << 1) - 1;
	else if (bestDelta < kNumFacings / 2)
		turn = 1;
	else
		turn = -1;

	facing += turn;
	return bestDelta;
}

}  // namespace uqm::sim
