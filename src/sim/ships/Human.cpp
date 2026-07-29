// Copyright the Ur-Quan Masters contributors. GPL-2.0-or-later.

#include "Human.hpp"

#include "engine/core/Types.hpp"
#include "sim/Battle.hpp"
#include "sim/Damage.hpp"
#include "sim/ShipSystems.hpp"
#include "sim/World.hpp"

#include <cassert>
#include <utility>

namespace uqm::sim {

usize
spawnCruiserPrimary(const ShipView &ship, std::span<Spawn> out) noexcept
{
	assert(!out.empty() && "spawn buffer must have room");

	Spawn &s = out[0];
	s.position = muzzlePosition(ship);
	s.facing = ship.facing;
	s.speed = ship.weaponSpeed;
	s.life = ship.weaponLife;
	s.damage = ship.weaponDamage;
	s.hitPoints = ship.weaponHitPoints;
	s.blastOffset = ship.blastOffset;
	s.playerNr = ship.playerNr;

	// human.c:281 -- `face = index = ShipFacing`. The nuke sprite is
	// directional, so its frame follows the facing.
	s.frameIndex = static_cast<u16>(s.facing.raw());

	// human.c:283 -- flags = 0. Cruiser missiles collide with each other.
	s.ignoreSimilar = false;
	return 1;
}

void
cruiserSpecial(Battle &b, EntityId id) noexcept
{
	const Allegiance *shipAllegiance = b.find<Allegiance>(id);
	if (shipAllegiance == nullptr)
		return;
	ShipState *sp = b.ship(id);
	if (sp == nullptr)
		return;

	const ShipSpec &spec = *sp->spec;
	const i32 range = spec.special.pointDefenceRange;
	if (range <= 0)
		return;

	const Vec2i from = b.get<Position>(id).next;
	bool paid = false;
	bool cannotAfford = false;

	// Every shot in range, not just the nearest: the C walks the whole list
	// and fires at each, paying once for the volley (human.c:225-236) -- a
	// Cruiser surrounded by fire clears all of it, or none if it can't afford
	// it. Physique and Position as a required join, not a get-then-null-
	// check (review-007 W4b's join rule): existence was only ever read for
	// its own sake, which Physique/Position presence already implies (both
	// attach at every Battle::spawn call).
	b.eachOrdered<Physique, Position>([&](EntityId other, Physique &otherPhys,
											  Position &otherPos) {
		if (cannotAfford || shipAllegiance == nullptr || other == id)
			return;

		if (!b.collidable(other))
			return;
		if (b.has<Cloaked>(other))
			return;  // human.c:203-204

		// No ownership test -- the C has none (human.c:203-204): the Cruiser pays
		// for and shoots down its OWN in-flight nukes in range, a real tactical
		// constraint (review-001 A15).

		// A deliberate divergence from the C, which will fire on a planet that
		// just absorbs it (do_damage exempts gravity masses) -- see design-notes V4.
		if (isGravityMass(otherPhys.mass))
			return;

		const Vec2i tNext = otherPos.next;
		const Vec2i dv = wrapDelta(tNext - from);
		const i32 dx = worldToDisplay(dv.x < 0 ? -dv.x : dv.x);
		const i32 dy = worldToDisplay(dv.y < 0 ? -dv.y : dv.y);
		if (dx > range || dy > range || dx * dx + dy * dy > range * range)
			return;

		if (!paid)
		{
			if (!deltaEnergy(*sp, -spec.special.energyCost))
			{
				cannotAfford = true;  // cannot afford it, so nothing burns
				return;
			}
			sp->specialCounter = spec.special.wait;
			paid = true;
		}

		doDamage(b, other, 1, id);

		// The beam is decorative -- only the damage above is real -- deterministic
		// geometry, not the renderer's (design-notes V3). LASER_LIFE is 1
		// (weapon.c:52); Beam{from,to} carries the ends-not-motion contract,
		// and it is the only component holding this entity's geometry at all
		// (review-007 W4a: a beam has no Position).
		const Vec2i beamTo = tNext;

		// Queued, not spawned: it enters the world at the sync point and
		// draws its one frame of life the step after this one -- the PD
		// beam is one frame later than the C's same-step catch-up gave it
		// (review-006 §4's accepted latency).
		b.queueSpawn(SpawnCommand{
				.layer = Layer::Ordnance,
				.allegiance = Allegiance{shipAllegiance->playerNr, id},
				.beam = Beam{from, beamTo},
				.lifetime = Lifetime{1},
		});

		shipAllegiance = b.find<Allegiance>(id);
	});
}

const ShipSpec &
earthlingCruiser() noexcept
{
	// human.c:26-55. Speeds store post-DISPLAY_TO_WORLD values; offsets store
	// raw display pixels (review-002 §5's spec-authoring rule).
	static const ShipSpec data{
		.maxCrew = 18,
		.thrust{.max = 24, .increment = 3},
		.battery{.max = 18, .regen = 1, .wait = 8},
		.thrustWait = 4,
		.turnWait = 1,
		.mass = 6,
		.weapon{
			.wait = 10,
			.energyCost = 9,
			.speed = 40,  // max(MAX_THRUST, DISPLAY_TO_WORLD(10)), human.c:42-45
			.muzzleOffset = 42,  // HUMAN_OFFSET
			.lifetime{.remaining = 60},
			.vitality{.hitPoints = 1},
			.warhead{.damage = 4, .blastOffset = 8},  // NUKE_OFFSET
			// Guided and accelerating (human.c:43-50): TRACK_WAIT 3,
			// DISPLAY_TO_WORLD(20) == 80, DISPLAY_TO_WORLD(1) == 4. The
			// clock starts already wound to trackWait (human.c:297-299).
			.guided = Guided{
					.trackWait = 3, .maxSpeed = 80, .thrustScale = 4, .clock = 3},
			.spawn = spawnCruiserPrimary,
		},
		.special{
			.wait = 9,
			.energyCost = 4,
			.hook = cruiserSpecial,
			.pointDefenceRange = 100,  // LASER_RANGE, display px
		},
	};
	return data;
}

}  // namespace uqm::sim
