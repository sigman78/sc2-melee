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
			// Out of crew. The ship does not vanish -- it becomes its own
			// explosion and burns for three dozen frames (ship_death ->
			// StartShipExplosion, tactrans.c:730-750). Setting life_span to 0
			// here, which is what this did before, deleted the ship on the
			// spot and made a kill look like a rendering glitch.
			startShipExplosion(e);
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

	// The weapon is spent. NOT quite "either way" in the C: weapon.c:161-164
	// keeps a weapon flying when its hit_points exceed a surviving finite
	// target's mass_points -- a pierce rule no M1 weapon can trigger (both
	// have 1 hit point), recorded as review-001 A16 and deferred with the
	// collision-protocol work rather than half-ported here. Re-fetched
	// because doDamage may have run a death hook that touched the list.
	w = b.get(id);
	if (w == nullptr)
		return;

	const Vec2i at = w->next;
	const int angle = w->velocity.travelAngle();
	const std::int32_t blastOffset = w->blastOffset;

	w->hitPoints = 0;
	w->lifeSpan = 0;
	// DISAPPEARING too (weapon.c:175-177): a spent missile is reaped this
	// same frame, never drawn dead at the impact point. The flame's wrapper
	// clears the flag again so the fireball lingers one frame
	// (flameCollision; ilwrath.c:141-148).
	w->flags |= ElementFlags::NonSolid | ElementFlags::Disappearing;

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
	// Tail, like every PutElement in the C: the post walk's catch-up ages it
	// this frame, so its five frames start now rather than next step.
	b.spawnBack(std::move(blast));
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

	// ship.c:364-367 -- `damage = hit_points >> 2`, floored at one. The trap:
	// for a PLAYER_SHIP, `hit_points` IS `crew_level` -- they are one union
	// field (element.h:126-133) -- so flying an 18-crew Cruiser into the
	// planet costs 4 crew, not 1. Reading a separate hitPoints field here,
	// which is what this did first, made every planet impact cost exactly 1
	// because a ship's own hitPoints is never assigned.
	//
	// Porting rule this bought: every C `hit_points` read on a player ship
	// is a crew read.
	const std::int32_t own = any(e->flags & ElementFlags::PlayerShip)
			? e->ship.crew
			: e->hitPoints;
	std::int32_t damage = own >> 2;
	if (damage == 0)
		damage = 1;
	doDamage(*e, damage);
}

}  // namespace uqm::sim
