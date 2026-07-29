// Copyright the Ur-Quan Masters contributors. GPL-2.0-or-later.

#include "Field.hpp"

#include "engine/core/Types.hpp"
#include "sim/Battle.hpp"
#include "sim/Damage.hpp"
#include "sim/Gravity.hpp"
#include "sim/Trig.hpp"
#include "sim/World.hpp"

#include <utility>

namespace uqm::sim {
namespace {

// DISPLAY_ALIGN_X/Y (units.h:85-86): the random word truncates to 16 bits
// (COUNT) before folding into the arena and snapping to a pixel -- the
// truncated *value* differs from a 31-bit modulo, and replays care about
// values.
[[nodiscard]] i32 displayAlignX(u32 r) noexcept
{
	return static_cast<i32>(
			(static_cast<u16>(r) % kArena.w) & ~(kScaledOne - 1));
}

[[nodiscard]] i32 displayAlignY(u32 r) noexcept
{
	return static_cast<i32>(
			(static_cast<u16>(r) % kArena.h) & ~(kScaledOne - 1));
}

// The rubble's own death hook: put a fresh asteroid back into the field.
// Queued as a deferred command, not an ordinary one: spawnAsteroid draws
// its own RNG sequence, and that draw must happen at the sync point in
// queue order -- drawing early would desynchronise the field's RNG stream
// from whatever else the sync point still draws this frame.
void rubbleDeath(Battle &b, EntityId id) noexcept
{
	const comp::StashedMask *rm = b.reg.try_get<comp::StashedMask>(id);
	const CollisionMask *mask = rm != nullptr ? rm->mask : nullptr;

	b.queueSpawn(SpawnCommand{
			.deferred =
					[](Battle &bb, Borrowed<const CollisionMask> m) noexcept {
						(void)spawnAsteroid(bb, m);
					},
			.deferredMask = mask,
	});
}

}  // namespace

bool timeSpaceMatterConflict(Battle &b, EntityId id)
{
	const comp::Collider *selfCollider = b.reg.try_get<comp::Collider>(id);
	if (!b.alive(id) || selfCollider == nullptr)
		return false;

	const Vec2i selfAt = b.reg.get<comp::Position>(id).current;
	const Body a{selfCollider->mask, selfAt, selfAt};

	// Order-independent: a plain OR over every other element, so the walk
	// need not be the spine -- an unordered view is enough, and keeps
	// entt::registry out of this file.
	bool conflict = false;
	b.reg.view<comp::Collider, comp::Position>().each(
			[&](EntityId other, comp::Collider &tCollider,
					comp::Position &pos) {
				if (conflict || other == id)
					return;

				// A player ship counts even when it is not collidable --
				// gravity.c:175 calls that "ship in transition", to stop a
				// planet materialising on top of a ship still warping in.
				if (!b.collidable(other)
						&& !b.reg.all_of<comp::ShipState>(other))
					return;

				const Body other_{tCollider.mask, pos.current, pos.current};
				if (sweptIntersect(a, other_))
					conflict = true;
			});

	return conflict;
}

void placeShipAtRandom(Battle &b, EntityId id, i32 minSeparation)
{
	if (!b.alive(id))
		return;

	// Far enough from every *other* ship, across the wrap. Squared, so there
	// is no square root and no overflow worry: the arena is 8192 across, so
	// the largest separation squared still fits an int32 with room to spare.
	const auto farEnough = [&b, id](i32 want) {
		if (want <= 0)
			return true;
		const Vec2i selfAt = b.reg.get<comp::Position>(id).current;
		bool tooClose = false;
		// ShipState is a required join too, but left as a manual has<>: a
		// full join would force binding a ShipState& this loop never uses.
		b.reg.view<comp::Position>().each(
				[&](EntityId other, comp::Position &pos) {
					if (tooClose || other == id
							|| !b.reg.all_of<comp::ShipState>(other))
						return;
					const Vec2i d = wrapDelta(pos.current - selfAt);
					if (d.x * d.x + d.y * d.y < want * want)
						tooClose = true;
				});
		return !tooClose;
	};

	for (int tries = 0;; ++tries)
	{
		comp::Position &pos = b.reg.get<comp::Position>(id);
		pos.current = wrap(Vec2i{
				displayAlignX(b.rng().next()), displayAlignY(b.rng().next())});
		pos.next = pos.current;

		if (calculateGravity(b, id) || timeSpaceMatterConflict(b, id))
			continue;

		// Give up on the separation rather than spin: past this many tries,
		// take any spot the C itself would have taken. A guarantee that can
		// hang is worse than a preference that can be missed.
		if (farEnough(tries < 500 ? minSeparation : 0))
			return;
	}
}

EntityId spawnPlanet(Battle &b, const CollisionMask *mask)
{
	// Motion defaults to zero; the planet never moves. Allegiance defaults
	// to NEUTRAL_PLAYER_NUM/no owner too, so spawn() below gets no explicit
	// one.

	// Mass is assigned only *after* placement (misc.c:71): while the loop runs
	// the planet isn't yet a gravity source, so calculateGravity asks only
	// "is this spot inside someone else's well?" -- which is what rejects it.
	Spawned s = b.spawn(Layer::Field, comp::Position{}, comp::Motion{},
			comp::Physique{}, mask);
	const EntityId id = s.id();

	// misc.c:55's lifeSpan = NORMAL_LIFE+1 encoded indestructibility as a
	// magic countdown value; the tag says it directly (Entity.hpp). Only
	// ever one Planet, so its identity is worth stating once, at spawn.
	s.with(comp::Indestructible{})
			.with(comp::Planet{})
			.with(comp::Vitality{200});

	do
	{
		comp::Position &pos = b.reg.get<comp::Position>(id);
		pos.current = wrap(Vec2i{
				displayAlignX(b.rng().next()), displayAlignY(b.rng().next())});
		pos.next = pos.current;
	} while (calculateGravity(b, id) || timeSpaceMatterConflict(b, id));

	b.reg.get<comp::Physique>(id).mass =
			b.reg.get<comp::Vitality>(id).hitPoints;
	return id;
}

EntityId spawnAsteroid(Battle &b, const CollisionMask *mask)
{
	// Allegiance defaults to NEUTRAL_PLAYER_NUM/no owner, same as the planet.
	const comp::Physique phys{
			3};  // NORMAL_LIFE, persistent: no Lifetime at all

	// Six draws, in the order misc.c:156-191 makes them -- not stylistic:
	// getting the sequence wrong desynchronised network play there, and would
	// desynchronise a replay here.
	comp::Position pos;
	const u32 edge = b.rng().next();
	if ((edge & (1u << 0)) != 0)
	{
		pos.current.x = (edge & (1u << 1)) != 0 ? kArena.w : 0;
		pos.current.y = wrapY(displayAlignY(b.rng().next()));
	}
	else
	{
		pos.current.x = wrapX(displayAlignX(b.rng().next()));
		pos.current.y = (edge & (1u << 1)) != 0 ? kArena.h : 0;
	}

	const i32 magnitude =
			displayToWorld(static_cast<i32>(b.rng().next() & 7) + 4);
	comp::Motion motion;
	motion.velocity.setVector(
			magnitude, Facing(static_cast<int>(b.rng().next())));

	pos.facing = Facing(static_cast<int>(b.rng().next()));

	// Draws six and seven, in the C's order and truncations (misc.c:156-193):
	// the period, then the direction bit. The values and their draw order
	// are pinned; how they're packed in memory is not.
	comp::Spin spin;
	spin.period = static_cast<i32>(b.rng().next() & 3);
	spin.countdown = spin.period;
	spin.backwards = (b.rng().next() & (1u << 7)) != 0;

	pos.next = pos.current;
	Spawned s = b.spawn(Layer::Field, pos, motion, phys, mask);
	s.with(spin).with(comp::Vitality{1}).with(comp::DeathSpawn{asteroidDeath});

	// A standing copy of the birth mask, independent of the Collider: a kill
	// detaches the Collider immediately, one frame or more before
	// asteroidDeath runs, but asteroidDeath still needs the mask to hand on.
	s.with(comp::StashedMask{mask});
	return s;
}

void asteroidDeath(Battle &b, EntityId id) noexcept
{
	if (!b.alive(id))
		return;

	const comp::StashedMask *deadMask = b.reg.try_get<comp::StashedMask>(id);
	auto [deadPos, deadAllegiance] =
			b.reg.get<comp::Position, comp::Allegiance>(id);

	comp::Position rPos;
	rPos.current = deadPos.current;
	rPos.next = rPos.current;

	// Queued, not spawned: it enters the world at the sync point and acts
	// next frame. The rubble itself never collides -- rubbleMask just
	// carries the asteroid's mask through to the replacement asteroid.
	b.queueSpawn(SpawnCommand{
			.layer = Layer::Ordnance,
			.position = rPos,
			.allegiance = comp::Allegiance{deadAllegiance.playerNr, kNoEntity},
			.effect = true,  // stationary: no Motion needed
			.blast = true,
			.lifetime = comp::Lifetime{5},
			.rubbleMask = deadMask != nullptr ? deadMask->mask : nullptr,
			.deathSpawn = rubbleDeath,
	});
}

}  // namespace uqm::sim
