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
	auto ship = b.get(id);
	if (ship == nullptr)
		return;
	ShipState *sp = b.ship(id);
	if (sp == nullptr)
		return;

	const ShipSpec &spec = *sp->spec;
	const i32 range = spec.special.pointDefenceRange;
	if (range <= 0)
		return;

	const Vec2i from = ship->next;
	bool paid = false;

	// Every shot in range, not just the nearest: the C walks the whole list
	// and fires at each, paying once for the volley (human.c:225-236) -- a
	// Cruiser surrounded by fire clears all of it, or none if it can't afford it.
	for (EntityId other = b.front(); other != kNoEntity;
			other = b.next(other))
	{
		if (other == id)
			continue;

		auto t = b.get(other);
		if (t == nullptr || !t->collidable())
			continue;
		if (isCloaked(b, other))
			continue;  // human.c:203-204

		// No ownership test -- the C has none (human.c:203-204): the Cruiser pays
		// for and shoots down its OWN in-flight nukes in range, a real tactical
		// constraint (review-001 A15).

		// A deliberate divergence from the C, which will fire on a planet that
		// just absorbs it (do_damage exempts gravity masses) -- see design-notes V4.
		if (isGravityMass(t->mass))
			continue;

		const Vec2i dv = wrapDelta(
				Vec2i{t->next.x - from.x, t->next.y - from.y});
		const i32 dx = worldToDisplay(dv.x < 0 ? -dv.x : dv.x);
		const i32 dy = worldToDisplay(dv.y < 0 ? -dv.y : dv.y);
		if (dx > range || dy > range || dx * dx + dy * dy > range * range)
			continue;

		if (!paid)
		{
			if (!deltaEnergy(*sp, -spec.special.energyCost))
				return;  // cannot afford it, so nothing burns
			sp->specialCounter = spec.special.wait;
			paid = true;
		}

		doDamage(b, other, 1);

		// The beam is decorative -- only the damage above is real -- deterministic
		// geometry, not the renderer's (design-notes V3). LASER_LIFE is 1
		// (weapon.c:52); BeamGeometry carries the ends-not-motion contract.
		const Vec2i beamTo = t->next;
		Element beam;
		beam.kind = ElementKind::Laser;
		beam.playerNr = ship->playerNr;
		beam.owner = id;
		beam.flags = ElementFlags::FiniteLife | ElementFlags::NonSolid;
		beam.lifeSpan = 1;
		beam.current = from;
		beam.next = beamTo;
		// Tail insertion: the post walk's catch-up reaches it this frame, so
		// its one frame of life is spent -- and drawn -- on the frame it was
		// fired, not the one after. BeamGeometry must be tagged before that
		// catch-up runs, which this statement order guarantees.
		const EntityId beamId = b.spawn(Layer::Ordnance, std::move(beam));
		b.attach<IgnoreVelocity>(beamId);
		b.attach<BeamGeometry>(beamId);

		ship = b.get(id);
		if (ship == nullptr)
			return;
	}
}

const ShipSpec &
earthlingCruiser() noexcept
{
	// human.c:26-55. Speeds store post-DISPLAY_TO_WORLD values; offsets store
	// raw display pixels (review-002 §5's spec-authoring rule).
	static const ShipSpec data{
		.maxCrew = 18,
		.maxEnergy = 18,
		.energyRegen = 1,
		.energyWait = 8,
		.thrust{.max = 24, .increment = 3},
		.thrustWait = 4,
		.turnWait = 1,
		.mass = 6,
		.weapon{
			.wait = 10,
			.energyCost = 9,
			.speed = 40,  // max(MAX_THRUST, DISPLAY_TO_WORLD(10)), human.c:42-45
			.life = 60,
			.damage = 4,
			.hitPoints = 1,
			.muzzleOffset = 42,  // HUMAN_OFFSET
			.blastOffset = 8,    // NUKE_OFFSET
			// Guided and accelerating (human.c:43-50): TRACK_WAIT 3,
			// DISPLAY_TO_WORLD(20) == 80, DISPLAY_TO_WORLD(1) == 4.
			.trackWait = 3,
			.maxSpeed = 80,
			.thrustScale = 4,
			.spawn = spawnCruiserPrimary,
			.preProcess = guidedShotPreProcess,
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
