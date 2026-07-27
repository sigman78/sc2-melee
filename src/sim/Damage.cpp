// Copyright the Ur-Quan Masters contributors. GPL-2.0-or-later.

#include "Damage.hpp"

#include "sim/Battle.hpp"
#include "sim/Ship.hpp"
#include "sim/Trig.hpp"
#include "sim/World.hpp"

#include <utility>

namespace uqm::sim {

bool
deltaCrew(ShipState &s, std::int32_t delta) noexcept
{
	if (delta > 0)
	{
		s.crew += delta;
		if (s.spec != nullptr && s.crew > s.spec->maxCrew)
			s.crew = s.spec->maxCrew;
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
doDamage(Battle &b, EntityId id, std::int32_t damage) noexcept
{
	auto e = b.get(id);
	if (e == nullptr)
		return;

	if (b.has<PlayerShip>(id))
	{
		ShipState *s = b.ship(id);
		if (s == nullptr)
			return;
		if (!deltaCrew(*s, -damage))
		{
			// Out of crew: the ship becomes its own explosion and burns for
			// three dozen frames (ship_death -> StartShipExplosion,
			// tactrans.c:730-750), rather than vanishing.
			startShipExplosion(b, id);
		}
		return;
	}

	// A gravity mass is not damageable. Asked without gravity.c's `+ 1`, so a
	// planet is immune and a fleeing ship at mass 100 is not.
	if (isGravityMass(e->mass))
		return;

	if (damage < e->hitPoints)
	{
		e->hitPoints -= damage;
		return;
	}
	e->hitPoints = 0;
	e->lifeSpan = 0;
	e->flags |= ElementFlags::NonSolid;
}

void
weaponCollision(Battle &b, EntityId id) noexcept
{
	auto w = b.get(id);
	if (w == nullptr)
		return;

	// "if already did effect" (weapon.c:141-142): a weapon that has raised
	// its own Collided this frame is done, however many partners the walk
	// still pairs it with.
	if (any(w->flags & ElementFlags::Collided))
		return;

	auto target = b.get(w->collidedWith);
	if (target == nullptr)
		return;
	const EntityId targetId = w->collidedWith;

	// Damage IS the weapon's mass (weapon.c:144) -- one number, two uses.
	const std::int32_t damage = w->mass;

	// weapon.c:145-158: hurts anything transient or at NORMAL_LIFE (excludes
	// something already dying). A target that SURVIVES marks the weapon
	// Collided ("did effect"), which also stops it at the impact point.
	if (damage > 0
			&& (any(target->flags & ElementFlags::FiniteLife)
					|| target->lifeSpan == 1))
	{
		doDamage(b, targetId, damage);
		w = b.get(id);
		target = b.get(targetId);
		if (w == nullptr)
			return;
		std::int32_t left = 0;
		if (target != nullptr)
		{
			const ShipState *ts = b.ship(targetId);
			left = ts != nullptr ? ts->crew : target->hitPoints;
		}
		if (left > 0)
			w->flags |= ElementFlags::Collided;
	}

	// Dies here against a solid target, always; against a finite one, only if
	// it hasn't already stopped and isn't tough enough to pierce -- hit points
	// vs. victim's mass (weapon.c:161-164; Chmmr zapsats pierce, nuke/flame don't).
	if (target != nullptr
			&& any(target->flags & ElementFlags::FiniteLife)
			&& (any(target->flags & ElementFlags::Collided)
					|| w->hitPoints > target->mass))
		return;

	const Vec2i at = w->next;
	const int angle = w->velocity.travelAngle();
	const std::int32_t blastOffset = w->blastOffset;

	w->hitPoints = 0;
	w->lifeSpan = 0;
	// COLLISION | NONSOLID | DISAPPEARING (weapon.c:175-181): stopped, spent,
	// reaped this frame. The flame's wrapper clears Disappearing again so the
	// fireball lingers one frame (flameCollision, ilwrath.c:141-148).
	w->flags |= ElementFlags::Collided | ElementFlags::NonSolid
			| ElementFlags::Disappearing;

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
	b.spawn(Layer::Ordnance, std::move(blast));
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

	// Hitting anything solid stops this element at the impact point
	// (ship.c:358 raises COLLISION for any non-finite other) -- which is what
	// makes solid-on-solid exchange momentum in the step loop.
	e->flags |= ElementFlags::Collided;

	if (!isGravityMass(other->mass))
		return;

	// ship.c:364-367: damage = hit_points >> 2, floored at one. For a
	// PLAYER_SHIP, hit_points IS crew_level (one union field, element.h:126-133)
	// -- every C hit_points read on a player ship is a crew read.
	const ShipState *ss = b.ship(id);
	const std::int32_t own = ss != nullptr ? ss->crew : e->hitPoints;
	std::int32_t damage = own >> 2;
	if (damage == 0)
		damage = 1;
	doDamage(b, id, damage);
}

}  // namespace uqm::sim
