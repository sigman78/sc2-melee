// Copyright the Ur-Quan Masters contributors. GPL-2.0-or-later.

#include "Damage.hpp"

#include "engine/core/Types.hpp"
#include "sim/Battle.hpp"
#include "sim/Ship.hpp"
#include "sim/Trig.hpp"
#include "sim/World.hpp"

#include <utility>

namespace uqm::sim {

bool
deltaCrew(ShipState &s, i32 delta) noexcept
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

namespace {

// emplace_or_accumulate (review-006 §2): the first hit this frame attaches
// the component, every later one just adds to it, so several sources
// stack into one summed application at the sync point.
void
accumulateDamage(Battle &b, EntityId id, i32 amount, EntityId from) noexcept
{
	if (DamageIncoming *di = b.find<DamageIncoming>(id))
	{
		di->amount += amount;
		di->lastFrom = from;
	}
	else
	{
		b.attach<DamageIncoming>(id, DamageIncoming{amount, from});
	}
}

}  // namespace

void
doDamage(Battle &b, EntityId id, i32 damage, EntityId from) noexcept
{
	auto e = b.get(id);
	if (e == nullptr)
		return;

	if (b.has<PlayerShip>(id))
	{
		if (b.ship(id) == nullptr)
			return;
		// Z4: a crewed hull no longer loses crew here -- it stacks in
		// DamageIncoming and Battle::step's sync point sums every source
		// this frame into one deltaCrew call and one death check.
		accumulateDamage(b, id, damage, from);
		return;
	}

	// A gravity mass is not damageable. Asked without gravity.c's `+ 1`, so a
	// planet is immune and a fleeing ship at mass 100 is not.
	if (isGravityMass(b.find<Physique>(id)->mass))
		return;

	if (damage < e->hitPoints)
	{
		e->hitPoints -= damage;
		return;
	}
	e->hitPoints = 0;
	// Death detected next frame (ageAndReapMarkPass), not this one -- no
	// Doomed here, matching lifeSpan = 0 without Disappearing (misc.c:210,221
	// has no FINITE_LIFE guard: this kills a non-aging asteroid the same way).
	b.attachOrReplace<Lifetime>(id, Lifetime{0});
	b.detach<Collider>(id);
}

void
weaponCollision(Battle &b, EntityId id) noexcept
{
	auto w = b.get(id);
	if (w == nullptr)
		return;
	CollisionScratch &wScratch = *b.find<CollisionScratch>(id);

	// "if already did effect" (weapon.c:141-142): a weapon that has raised
	// its own Collided this frame is done, however many partners the walk
	// still pairs it with.
	if (wScratch.collided)
		return;

	auto target = b.get(w->collidedWith);
	if (target == nullptr)
		return;
	const EntityId targetId = w->collidedWith;
	CollisionScratch &targetScratch = *b.find<CollisionScratch>(targetId);

	// Damage IS the weapon's mass (weapon.c:144) -- one number, two uses.
	const i32 damage = b.find<Physique>(id)->mass;

	// weapon.c:145-158: hurts anything transient or at NORMAL_LIFE (excludes
	// something already dying). A target that SURVIVES marks the weapon
	// Collided ("did effect"), which also stops it at the impact point.
	if (damage > 0
			&& (isFiniteLife(b, targetId) || lifeSpanOf(b, targetId) == 1))
	{
		doDamage(b, targetId, damage, w->owner);
		w = b.get(id);
		target = b.get(targetId);
		if (w == nullptr)
			return;
		i32 left = 0;
		if (target != nullptr)
		{
			const ShipState *ts = b.ship(targetId);
			left = ts != nullptr ? ts->crew : target->hitPoints;
		}
		if (left > 0)
			wScratch.collided = true;
	}

	// Dies here against a solid target, always; against a finite one, only if
	// it hasn't already stopped and isn't tough enough to pierce -- hit points
	// vs. victim's mass (weapon.c:161-164; Chmmr zapsats pierce, nuke/flame don't).
	if (target != nullptr
			&& isFiniteLife(b, targetId)
			&& (targetScratch.collided
					|| w->hitPoints > b.find<Physique>(targetId)->mass))
		return;

	const Vec2i at = b.find<Position>(id)->next;
	const int angle = b.find<Motion>(id)->velocity.travelAngle();
	const i32 blastOffset = w->blastOffset;

	w->hitPoints = 0;
	// NONSOLID | DISAPPEARING (weapon.c:175-181), Collided in scratch:
	// stopped, spent, reaped this frame. The flame's wrapper clears Doomed
	// again so the fireball lingers one frame (flameCollision,
	// ilwrath.c:141-148).
	wScratch.collided = true;
	b.attachOrReplace<Lifetime>(id, Lifetime{0});
	b.attachOrReplace<Doomed>(id);
	b.detach<Collider>(id);

	// The blast, offset along the direction of travel so it sits on the
	// surface it hit rather than inside it (weapon.c:198-208).
	Element blast;
	blast.kind = ElementKind::Blast;
	blast.playerNr = w->playerNr;
	Position blastPos;
	blastPos.current = wrap(Vec2i{at.x + cosine(angle, displayToWorld(blastOffset)),
			at.y + sine(angle, displayToWorld(blastOffset))});
	blastPos.next = blastPos.current;

	// Queued, not spawned: it enters the world at the sync point and acts
	// next frame, one frame later than the C's same-step catch-up gave it
	// (review-006 §4's accepted latency).
	SpawnCommand cmd;
	cmd.layer = Layer::Ordnance;
	cmd.element = std::move(blast);
	cmd.position = blastPos;
	cmd.lifetime = Lifetime{kBlastLife};
	b.queueSpawn(std::move(cmd));
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
	if (isFiniteLife(b, e->collidedWith))
		return;

	// Hitting anything solid stops this element at the impact point
	// (ship.c:358 raises COLLISION for any non-finite other) -- which is what
	// makes solid-on-solid exchange momentum in the step loop.
	b.find<CollisionScratch>(id)->collided = true;

	if (!isGravityMass(b.find<Physique>(e->collidedWith)->mass))
		return;

	// ship.c:364-367: damage = hit_points >> 2, floored at one. For a
	// PLAYER_SHIP, hit_points IS crew_level (one union field, element.h:126-133)
	// -- every C hit_points read on a player ship is a crew read.
	const ShipState *ss = b.ship(id);
	const i32 own = ss != nullptr ? ss->crew : e->hitPoints;
	i32 damage = own >> 2;
	if (damage == 0)
		damage = 1;
	doDamage(b, id, damage, e->collidedWith);
}

}  // namespace uqm::sim
