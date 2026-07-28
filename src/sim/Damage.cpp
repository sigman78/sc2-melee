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
	if (!b.alive(id))
		return;

	if (b.has<ShipState>(id))
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

	// Every non-ship collidable thing that can reach here carries a
	// Vitality (weapons, the asteroid field, the planet -- review-007
	// W4b's attach set); a bare id with neither ShipState nor Vitality
	// simply has nothing left to hurt.
	Vitality *v = b.find<Vitality>(id);
	if (v == nullptr)
		return;

	if (damage < v->hitPoints)
	{
		v->hitPoints -= damage;
		return;
	}
	v->hitPoints = 0;
	// Death detected next frame (ageAndReapMarkPass), not this one -- no
	// Doomed here, matching lifeSpan = 0 without Disappearing (misc.c:210,221
	// has no FINITE_LIFE guard: this kills a non-aging asteroid the same way).
	b.attachOrReplace<Lifetime>(id, Lifetime{0});
	b.detach<Collider>(id);
}

void
weaponCollision(Battle &b, EntityId id, EntityId targetId) noexcept
{
	if (!b.alive(id))
		return;
	CollisionScratch &wScratch = *b.find<CollisionScratch>(id);

	// "if already did effect" (weapon.c:141-142): a weapon that has raised
	// its own Collided this frame is done, however many partners the walk
	// still pairs it with.
	if (wScratch.collided)
		return;

	if (!b.alive(targetId))
		return;
	CollisionScratch &targetScratch = *b.find<CollisionScratch>(targetId);

	// Damage IS the weapon's mass (weapon.c:144) -- one number, two uses.
	const i32 damage = b.find<Physique>(id)->mass;

	// weapon.c:145-158: hurts anything transient or at NORMAL_LIFE (excludes
	// something already dying), except an Indestructible target -- the
	// planet, which weapon.c's magic lifeSpan=2 used to fail this test on
	// its own (review-007). A target that SURVIVES marks the weapon
	// Collided ("did effect"), which also stops it at the impact point.
	if (damage > 0 && !b.has<Indestructible>(targetId)
			&& (isFiniteLife(b, targetId) || lifeSpanOf(b, targetId) == 1))
	{
		doDamage(b, targetId, damage, b.find<Allegiance>(id)->owner);
		if (!b.alive(id))
			return;
		i32 left = 0;
		if (b.alive(targetId))
		{
			const ShipState *ts = b.ship(targetId);
			const Vitality *tv = b.find<Vitality>(targetId);
			left = ts != nullptr ? ts->crew : tv != nullptr ? tv->hitPoints : 0;
		}
		if (left > 0)
			wScratch.collided = true;
	}

	// Dies here against a solid target, always; against a finite one, only if
	// it hasn't already stopped and isn't tough enough to pierce -- hit points
	// vs. victim's mass (weapon.c:161-164; Chmmr zapsats pierce, nuke/flame don't).
	// The weapon's own hit points are its Vitality now, same as any other
	// non-ship collidable.
	Vitality *wVital = b.find<Vitality>(id);
	if (b.alive(targetId)
			&& isFiniteLife(b, targetId)
			&& (targetScratch.collided
					|| wVital->hitPoints > b.find<Physique>(targetId)->mass))
		return;

	auto [pos, motion, warhead] = b.get<Position, Motion, Warhead>(id);
	const Vec2i at = pos.next;
	const int angle = motion.velocity.travelAngle();

	wVital->hitPoints = 0;
	// NONSOLID | DISAPPEARING (weapon.c:175-181), Collided in scratch:
	// stopped, spent, reaped this frame.
	wScratch.collided = true;
	b.attachOrReplace<Lifetime>(id, Lifetime{0});
	b.attachOrReplace<Doomed>(id);
	b.detach<Collider>(id);

	// ilwrath.c:141-148: the flame's own bit undoes the death mark on the
	// spot, so the fireball still draws for the frame it died on instead of
	// vanishing at once.
	if (warhead.lingersOnHit)
		b.detach<Doomed>(id);

	// The blast, offset along the direction of travel so it sits on the
	// surface it hit rather than inside it (weapon.c:198-208). Inherits the
	// weapon's playerNr, no owner of its own (never set on the old Element
	// either).
	const i32 shooterPlayerNr = b.find<Allegiance>(id)->playerNr;
	Position blastPos;
	blastPos.current = wrap(Vec2i{at.x + cosine(angle, displayToWorld(warhead.blastOffset)),
			at.y + sine(angle, displayToWorld(warhead.blastOffset))});
	blastPos.next = blastPos.current;

	// Queued, not spawned: it enters the world at the sync point and acts
	// next frame, one frame later than the C's same-step catch-up gave it
	// (review-006 §4's accepted latency).
	SpawnCommand cmd;
	cmd.layer = Layer::Ordnance;
	cmd.position = blastPos;
	cmd.lifetime = Lifetime{kBlastLife};
	cmd.effect = true;  // stationary: no Motion needed (review-007 W5)
	cmd.blast = true;
	cmd.allegiance = Allegiance{shooterPlayerNr, kNoEntity};
	b.queueSpawn(std::move(cmd));
}

void
solidCollision(Battle &b, EntityId id, EntityId otherId) noexcept
{
	if (!b.alive(id))
		return;

	if (!b.alive(otherId))
		return;

	// ship.c:356. A transient thing hitting you is the weapon's business, not
	// yours -- it has its own response and has already run.
	if (isFiniteLife(b, otherId))
		return;

	// Hitting anything solid stops this element at the impact point
	// (ship.c:358 raises COLLISION for any non-finite other) -- which is what
	// makes solid-on-solid exchange momentum in the step loop.
	b.find<CollisionScratch>(id)->collided = true;

	if (!isGravityMass(b.find<Physique>(otherId)->mass))
		return;

	// ship.c:364-367: damage = hit_points >> 2, floored at one. For a
	// PLAYER_SHIP, hit_points IS crew_level (one union field, element.h:126-133)
	// -- every C hit_points read on a player ship is a crew read; anything
	// else that solidCollision ever runs for (asteroid, planet) has a
	// Vitality instead.
	const ShipState *ss = b.ship(id);
	const Vitality *v = b.find<Vitality>(id);
	const i32 own = ss != nullptr ? ss->crew : v != nullptr ? v->hitPoints : 0;
	i32 damage = own >> 2;
	if (damage == 0)
		damage = 1;
	doDamage(b, id, damage, otherId);
}

}  // namespace uqm::sim
