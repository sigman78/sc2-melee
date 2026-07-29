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

usize spawnCruiserPrimary(const ShipView &ship, std::span<Spawn> out) noexcept
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

void cruiserSpecial(Battle &b, EntityId id) noexcept
{
	const comp::Allegiance *shipAllegiance =
			b.reg.try_get<comp::Allegiance>(id);
	if (shipAllegiance == nullptr)
		return;
	comp::ShipState *sp = b.ship(id);
	if (sp == nullptr)
		return;

	const ShipSpec &spec = *sp->spec;
	const i32 range = spec.special.pointDefenceRange;
	if (range <= 0)
		return;

	const Vec2i from = b.reg.get<comp::Position>(id).next;
	bool paid = false;
	bool cannotAfford = false;

	// Every shot in range, not just the nearest: the C walks the whole list
	// and fires at each, paying once for the volley (human.c:225-236) -- a
	// Cruiser surrounded by fire clears all of it, or none if it can't afford
	// it.
	b.eachOrdered<comp::Physique, comp::Position>(
			[&](EntityId other, comp::Physique &otherPhys,
					comp::Position &otherPos) {
				if (cannotAfford || shipAllegiance == nullptr || other == id)
					return;

				if (!b.collidable(other))
					return;
				if (b.reg.all_of<comp::Cloaked>(other))
					return;  // human.c:203-204

				// No ownership test -- the C has none (human.c:203-204): the
				// Cruiser pays for and shoots down its OWN in-flight nukes in
				// range, a real tactical constraint.

				// A deliberate divergence from the C, which will fire on a
				// planet that just absorbs it (do_damage exempts gravity
				// masses).
				if (isGravityMass(otherPhys.mass))
					return;

				const Vec2i tNext = otherPos.next;
				const Vec2i dv = wrapDelta(tNext - from);
				const i32 dx = worldToDisplay(dv.x < 0 ? -dv.x : dv.x);
				const i32 dy = worldToDisplay(dv.y < 0 ? -dv.y : dv.y);
				if (dx > range || dy > range
						|| dx * dx + dy * dy > range * range)
					return;

				if (!paid)
				{
					if (!deltaEnergy(*sp, -spec.special.energyCost))
					{
						cannotAfford =
								true;  // cannot afford it, so nothing burns
						return;
					}
					sp->specialCounter = spec.special.wait;
					paid = true;
				}

				doDamage(b, other, 1, id);

				// The beam is decorative -- only the damage above is real,
				// deterministic geometry. LASER_LIFE is 1 (weapon.c:52);
				// Beam{from,to} carries the ends, not motion -- this entity has
				// no Position.
				const Vec2i beamTo = tNext;

				// In the walk at the sync point: it draws its one frame of
				// life the step after this one, one frame later than the C's
				// same-step catch-up gave it.
				b.spawnBeam(Layer::Ordnance, comp::Beam{from, beamTo},
						 comp::Allegiance{shipAllegiance->playerNr, id})
						.with(comp::Lifetime{1});

				shipAllegiance = b.reg.try_get<comp::Allegiance>(id);
			});
}

const ShipSpec &earthlingCruiser() noexcept
{
	// human.c:26-55. Speeds store post-DISPLAY_TO_WORLD values; offsets
	// store raw display pixels.
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
					.speed = 40,  // max(MAX_THRUST, DISPLAY_TO_WORLD(10)),
								  // human.c:42-45
					.muzzleOffset = 42,  // HUMAN_OFFSET
					.lifetime{.remaining = 60},
					.vitality{.hitPoints = 1},
					.warhead{.damage = 4, .blastOffset = 8},  // NUKE_OFFSET
					// Guided and accelerating (human.c:43-50): TRACK_WAIT 3,
					// DISPLAY_TO_WORLD(20) == 80, DISPLAY_TO_WORLD(1) == 4. The
					// clock starts already wound to trackWait
					// (human.c:297-299).
					.guided = comp::Guided{.trackWait = 3,
							.maxSpeed = 80,
							.thrustScale = 4,
							.clock = 3},
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
