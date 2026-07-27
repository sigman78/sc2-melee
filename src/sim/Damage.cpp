// Copyright the Ur-Quan Masters contributors. GPL-2.0-or-later.

#include "Damage.hpp"

#include "sim/Battle.hpp"
#include "sim/Ship.hpp"
#include "sim/Trig.hpp"
#include "sim/World.hpp"

#include <utility>

namespace uqm::sim {

bool
deltaCrew(Element &e, std::int32_t delta) noexcept
{
	ShipState &s = e.ship;
	if (delta > 0)
	{
		s.crew += delta;
		if (s.data != nullptr && s.crew > s.data->maxCrew)
			s.crew = s.data->maxCrew;
		return true;
	}

	// status.c:357-363. The C compares against the magnitude with a strict
	// `>`, so losing exactly the remaining crew reports failure -- a ship at 4
	// crew hit for 4 is destroyed, not left at zero and alive.
	if (s.crew > -delta)
	{
		s.crew += delta;
		return true;
	}
	s.crew = 0;
	return false;
}

void
doDamage(Element &e, std::int32_t damage) noexcept
{
	if (any(e.flags & ElementFlags::PlayerShip))
	{
		if (!deltaCrew(e, -damage))
		{
			// Out of crew. NONSOLID first, so nothing else can hit the wreck
			// on its way out, and life_span = 0 means the step loop reaps it
			// at the start of the next frame and runs its death hook.
			e.lifeSpan = 0;
			e.flags |= ElementFlags::NonSolid;
		}
		return;
	}

	// A gravity mass is not damageable. Asked without gravity.c's `+ 1`, so a
	// planet is immune and a fleeing ship at mass 100 is not.
	if (isGravityMass(e.mass))
		return;

	if (damage < e.hitPoints)
	{
		e.hitPoints -= damage;
		return;
	}
	e.hitPoints = 0;
	e.lifeSpan = 0;
	e.flags |= ElementFlags::NonSolid;
}

void
weaponCollision(Battle &b, EntityId id) noexcept
{
	auto w = b.get(id);
	if (w == nullptr)
		return;

	const EntityId targetId = w->collidedWith;
	const std::int32_t damage = w->damage;

	if (auto target = b.get(targetId); target != nullptr && damage > 0)
	{
		// weapon.c:145-147. A weapon hurts things that are either transient
		// themselves or are at NORMAL_LIFE -- which is everything that is
		// really on the field, and excludes something already dying.
		if (any(target->flags & ElementFlags::FiniteLife)
				|| target->lifeSpan == 1)
			doDamage(*target, damage);
	}

	// The weapon is spent either way (weapon.c:179-181). Re-fetched because
	// doDamage may have run a death hook that touched the list.
	w = b.get(id);
	if (w == nullptr)
		return;

	const Vec2i at = w->next;
	const int angle = w->velocity.travelAngle();
	const std::int32_t blastOffset = w->blastOffset;

	w->hitPoints = 0;
	w->lifeSpan = 0;
	w->flags |= ElementFlags::NonSolid;

	// The blast, offset along the direction of travel so it sits on the
	// surface it hit rather than inside it (weapon.c:198-208).
	Element blast;
	blast.kind = ElementKind::Blast;
	blast.playerNr = w->playerNr;
	blast.flags = ElementFlags::FiniteLife | ElementFlags::NonSolid;
	blast.lifeSpan = kBlastLife;
	blast.current = wrap(Vec2i{at.x + cosine(angle, displayToWorld(blastOffset)),
			at.y + sine(angle, displayToWorld(blastOffset))});
	blast.next = blast.current;
	b.spawnFront(std::move(blast));
}

void
solidCollision(Battle &b, EntityId id) noexcept
{
	auto e = b.get(id);
	if (e == nullptr)
		return;

	auto other = b.get(e->collidedWith);
	if (other == nullptr)
		return;

	// ship.c:356. A transient thing hitting you is the weapon's business, not
	// yours -- it has its own hook and has already run.
	if (any(other->flags & ElementFlags::FiniteLife))
		return;

	if (!isGravityMass(other->mass))
		return;

	// ship.c:364-367. In melee this is always 1: a player ship's hit_points is
	// never assigned, so it is zero, and an asteroid's is 1 -- either way the
	// shift yields 0 and the floor kicks in. The arithmetic is kept because it
	// is what the C computes, not because it has ever produced anything else.
	std::int32_t damage = e->hitPoints >> 2;
	if (damage == 0)
		damage = 1;
	doDamage(*e, damage);
}

}  // namespace uqm::sim
