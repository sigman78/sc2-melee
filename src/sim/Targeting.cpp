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
	const auto self = b.find<Allegiance>(tracker);
	if (self == nullptr)
		return -1;

	// Doc §2 refinement 1: GuidedSteer runs before Integrate now, so
	// `current` is the frame's one consistent snapshot for every tracker and
	// every target alike -- the old read-`next`-if-already-preprocessed
	// dance existed only because the interleaved walk moved half the list
	// before the other half looked.
	const Vec2i from = b.find<Position>(tracker)->current;

	int bestDelta = 0;
	i32 bestDistance = 0;
	EntityId bestTarget;
	bool found = false;

	// Allegiance, ShipState and Position as a required join, not a get-then-
	// null-check per iteration (review-007 W4b's join rule): every ship
	// has all three, so PlayerShip need not be checked separately either --
	// only a ship ever carries a ShipState (attachShip/spawnPlayerShip
	// attach both together). Dead-ship and cloak are value tests, so they
	// stay in the body.
	b.eachOrdered<Allegiance, ShipState, Position>([&](EntityId id,
															Allegiance &t,
															ShipState &ts,
															Position &pos) {
		if (t.playerNr == self->playerNr)
			return;
		// Dead ships are not targets (weapon.c:352-353).
		if (lifeSpanOf(b, id) == 0 || ts.crew == 0)
			return;
		// Nor cloaked ones (weapon.c:344-348). This is the whole tactical
		// point of the Ilwrath cloak: not that it is hard to see, but that a
		// guided weapon has nothing to steer toward.
		if (b.has<Cloaked>(id))
			return;

		const Vec2i to = pos.current;
		const Vec2i d = wrapDelta(to - from);
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
	});

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
