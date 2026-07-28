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
// truncated *value* differs from a 31-bit modulo, and replays care about values.
[[nodiscard]] i32
displayAlignX(u32 r) noexcept
{
	return static_cast<i32>(
			(static_cast<u16>(r) % kLogSpaceWidth) & ~(kScaledOne - 1));
}

[[nodiscard]] i32
displayAlignY(u32 r) noexcept
{
	return static_cast<i32>(
			(static_cast<u16>(r) % kLogSpaceHeight) & ~(kScaledOne - 1));
}

// asteroid_preprocess (misc.c:107-128): tumbles only, by its Spin
// component. The C's rotation lives in the sprite frame; here, with no
// sprite, in `facing`.
void
asteroidPreProcess(Battle &b, EntityId id) noexcept
{
	auto e = b.get(id);
	if (e == nullptr)
		return;
	Spin *spin = b.find<Spin>(id);
	if (spin == nullptr)
		return;

	if (spin->countdown > 0)
	{
		--spin->countdown;
		return;
	}

	b.find<Position>(id)->facing += spin->backwards ? -1 : 1;
	spin->countdown = spin->period;
}

// The rubble's own death hook: put a fresh asteroid back into the field.
//
// Queued as a deferred command rather than an ordinary one: spawnAsteroid
// draws its own RNG sequence building the Element and attaching Spin, and
// that draw has to happen at the sync point, in queue order, not here at
// emission (mid-pipeline, slot 2) -- drawing early would put the field's
// stream out of step with whatever else the sync point still draws or
// builds this frame.
void
rubbleDeath(Battle &b, EntityId id) noexcept
{
	const StashedMask *rm = b.find<StashedMask>(id);
	const CollisionMask *mask = rm != nullptr ? rm->mask : nullptr;

	SpawnCommand cmd;
	cmd.deferred = [](Battle &bb, Borrowed<const CollisionMask> m) noexcept {
		(void)spawnAsteroid(bb, m);
	};
	cmd.deferredMask = mask;
	b.queueSpawn(std::move(cmd));
}

}  // namespace

bool
timeSpaceMatterConflict(Battle &b, EntityId id)
{
	auto self = b.get(id);
	const Collider *selfCollider = b.find<Collider>(id);
	if (self == nullptr || selfCollider == nullptr)
		return false;

	const Vec2i selfAt = b.find<Position>(id)->current;
	const Body a{selfCollider->mask, selfAt, selfAt};

	// Order-independent: a plain OR over every other element, so the walk
	// need not be the spine -- eachElement is enough, and keeps
	// entt::registry out of this file.
	bool conflict = false;
	b.eachElement([&](EntityId other, Element &) {
		if (conflict || other == id)
			return;
		const Collider *tCollider = b.find<Collider>(other);
		if (tCollider == nullptr)
			return;

		// A player ship counts even when it is not collidable -- gravity.c:175
		// calls that case "ship in transition", and it is what stops a planet
		// materialising on top of a ship that is still warping in.
		if (!b.collidable(other) && !b.has<PlayerShip>(other))
			return;

		const Vec2i at = b.find<Position>(other)->current;
		const Body other_{tCollider->mask, at, at};
		if (sweptIntersect(a, other_))
			conflict = true;
	});

	return conflict;
}

void
placeShipAtRandom(Battle &b, EntityId id, i32 minSeparation)
{
	if (b.get(id) == nullptr)
		return;

	// Far enough from every *other* ship, across the wrap. Squared, so there
	// is no square root and no overflow worry: the arena is 8192 across, so
	// the largest separation squared still fits an int32 with room to spare.
	const auto farEnough = [&b, id](i32 want) {
		if (want <= 0)
			return true;
		const Vec2i selfAt = b.find<Position>(id)->current;
		bool tooClose = false;
		b.eachElement([&](EntityId other, Element &) {
			if (tooClose || other == id || !b.has<PlayerShip>(other))
				return;
			const Vec2i tAt = b.find<Position>(other)->current;
			const Vec2i d = wrapDelta(
					Vec2i{tAt.x - selfAt.x, tAt.y - selfAt.y});
			if (d.x * d.x + d.y * d.y < want * want)
				tooClose = true;
		});
		return !tooClose;
	};

	for (int tries = 0;; ++tries)
	{
		Position *pos = b.find<Position>(id);
		pos->current = wrap(Vec2i{displayAlignX(b.rng().next()),
				displayAlignY(b.rng().next())});
		pos->next = pos->current;

		if (calculateGravity(b, id) || timeSpaceMatterConflict(b, id))
			continue;

		// Give up on the separation rather than spin: past this many tries,
		// take any spot the C itself would have taken. A guarantee that can
		// hang is worse than a preference that can be missed.
		if (farEnough(tries < 500 ? minSeparation : 0))
			return;
	}
}

EntityId
spawnPlanet(Battle &b, const CollisionMask *mask)
{
	Element p;
	p.kind = ElementKind::Planet;
	p.playerNr = -1;             // NEUTRAL_PLAYER_NUM
	p.hitPoints = 200;
	p.onCollision = solidCollision;
	// Motion defaults to zero; the planet never moves.

	// Mass is assigned only *after* placement (misc.c:71): while the loop runs
	// the planet isn't yet a gravity source, so calculateGravity asks only
	// "is this spot inside someone else's well?" -- which is what rejects it.
	const EntityId id = b.spawn(
			Layer::Field, std::move(p), Position{}, Motion{}, Physique{}, mask);

	// misc.c:55's lifeSpan = NORMAL_LIFE+1 encoded indestructibility as a
	// magic countdown value; the tag says it directly (Entity.hpp).
	b.attach<Indestructible>(id);

	do
	{
		Position *pos = b.find<Position>(id);
		pos->current = wrap(Vec2i{displayAlignX(b.rng().next()),
				displayAlignY(b.rng().next())});
		pos->next = pos->current;
	} while (calculateGravity(b, id) || timeSpaceMatterConflict(b, id));

	b.find<Physique>(id)->mass = b.get(id)->hitPoints;
	return id;
}

EntityId
spawnAsteroid(Battle &b, const CollisionMask *mask)
{
	Element a;
	a.kind = ElementKind::Asteroid;
	a.playerNr = -1;
	a.hitPoints = 1;
	const Physique phys{3};      // NORMAL_LIFE, persistent: no Lifetime at all
	a.preProcess = asteroidPreProcess;
	a.onCollision = solidCollision;
	a.onDeath = asteroidDeath;

	// Six draws, in the order misc.c:156-191 makes them -- not stylistic:
	// getting the sequence wrong desynchronised network play there, and would
	// desynchronise a replay here.
	Position pos;
	const u32 edge = b.rng().next();
	if ((edge & (1u << 0)) != 0)
	{
		pos.current.x = (edge & (1u << 1)) != 0 ? kLogSpaceWidth : 0;
		pos.current.y = wrapY(displayAlignY(b.rng().next()));
	}
	else
	{
		pos.current.x = wrapX(displayAlignX(b.rng().next()));
		pos.current.y = (edge & (1u << 1)) != 0 ? kLogSpaceHeight : 0;
	}

	const i32 magnitude =
			displayToWorld(static_cast<i32>(b.rng().next() & 7) + 4);
	Motion motion;
	motion.velocity.setVector(
			magnitude, Facing(static_cast<int>(b.rng().next())));

	pos.facing = Facing(static_cast<int>(b.rng().next()));

	// Draws six and seven, in the C's order and truncations (misc.c:156-193):
	// the period, then the direction bit. They used to be bit-packed into
	// Element::thrustWait; the values and their draw order are pinned, the
	// packing is not.
	Spin spin;
	spin.period = static_cast<i32>(b.rng().next() & 3);
	spin.countdown = spin.period;
	spin.backwards = (b.rng().next() & (1u << 7)) != 0;

	pos.next = pos.current;
	const EntityId id =
			b.spawn(Layer::Field, std::move(a), pos, motion, phys, mask);
	b.attach<Spin>(id, spin);

	// A standing copy of the birth mask, independent of the Collider: a kill
	// (doDamage) detaches the Collider on the spot so the asteroid stops
	// colliding immediately, one frame or more before asteroidDeath runs --
	// this is what asteroidDeath still has to hand the rubble.
	b.attach<StashedMask>(id, StashedMask{mask});
	return id;
}

void
asteroidDeath(Battle &b, EntityId id) noexcept
{
	auto dead = b.get(id);
	if (dead == nullptr)
		return;

	const StashedMask *deadMask = b.find<StashedMask>(id);
	const Position *deadPos = b.find<Position>(id);

	Element r;
	r.kind = ElementKind::Blast;
	r.playerNr = dead->playerNr;
	r.turnWait = 0;
	r.onDeath = rubbleDeath;
	Position rPos;
	rPos.current = deadPos->current;
	rPos.next = rPos.current;

	// Queued, not spawned: it enters the world at the sync point and acts
	// next frame (review-006 §4's accepted one-frame latency), where the C's
	// tail insertion let the same frame's live walk still reach it. The
	// rubble itself never collides -- rubbleMask just carries the asteroid's
	// mask through its non-solid life, for rubbleDeath to hand to the
	// replacement asteroid.
	SpawnCommand cmd;
	cmd.layer = Layer::Ordnance;
	cmd.element = std::move(r);
	cmd.position = rPos;
	cmd.lifetime = Lifetime{5};
	cmd.rubbleMask = deadMask != nullptr ? deadMask->mask : nullptr;
	b.queueSpawn(std::move(cmd));
}

}  // namespace uqm::sim
