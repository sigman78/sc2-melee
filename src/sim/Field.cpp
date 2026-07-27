// Copyright the Ur-Quan Masters contributors. GPL-2.0-or-later.

#include "Field.hpp"

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
[[nodiscard]] std::int32_t
displayAlignX(std::uint32_t r) noexcept
{
	return static_cast<std::int32_t>(
			(static_cast<std::uint16_t>(r) % kLogSpaceWidth) & ~(kScaledOne - 1));
}

[[nodiscard]] std::int32_t
displayAlignY(std::uint32_t r) noexcept
{
	return static_cast<std::int32_t>(
			(static_cast<std::uint16_t>(r) % kLogSpaceHeight) & ~(kScaledOne - 1));
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

	e->facing += spin->backwards ? -1 : 1;
	spin->countdown = spin->period;
}

// The rubble's own death hook: put a fresh asteroid back into the field.
void
rubbleDeath(Battle &b, EntityId id) noexcept
{
	auto e = b.get(id);
	const CollisionMask *mask = (e != nullptr) ? e->mask : nullptr;
	(void)spawnAsteroid(b, mask);
}

}  // namespace

bool
timeSpaceMatterConflict(Battle &b, EntityId id)
{
	auto self = b.get(id);
	if (self == nullptr || self->mask == nullptr)
		return false;

	const Body a{self->mask, self->current, self->current};

	// Order-independent: a plain OR over every other element, so the walk
	// need not be the spine -- eachElement is enough, and keeps
	// entt::registry out of this file.
	bool conflict = false;
	b.eachElement([&](EntityId other, Element &t) {
		if (conflict || other == id || t.mask == nullptr)
			return;

		// A player ship counts even when it is not collidable -- gravity.c:175
		// calls that case "ship in transition", and it is what stops a planet
		// materialising on top of a ship that is still warping in.
		if (!t.collidable() && !b.has<PlayerShip>(other))
			return;

		const Body other_{t.mask, t.current, t.current};
		if (sweptIntersect(a, other_))
			conflict = true;
	});

	return conflict;
}

void
placeShipAtRandom(Battle &b, EntityId id, std::int32_t minSeparation)
{
	if (b.get(id) == nullptr)
		return;

	// Far enough from every *other* ship, across the wrap. Squared, so there
	// is no square root and no overflow worry: the arena is 8192 across, so
	// the largest separation squared still fits an int32 with room to spare.
	const auto farEnough = [&b, id](std::int32_t want) {
		if (want <= 0)
			return true;
		auto self = b.get(id);
		bool tooClose = false;
		b.eachElement([&](EntityId other, Element &t) {
			if (tooClose || other == id || !b.has<PlayerShip>(other))
				return;
			const Vec2i d = wrapDelta(Vec2i{t.current.x - self->current.x,
					t.current.y - self->current.y});
			if (d.x * d.x + d.y * d.y < want * want)
				tooClose = true;
		});
		return !tooClose;
	};

	for (int tries = 0;; ++tries)
	{
		auto e = b.get(id);
		e->current = wrap(Vec2i{displayAlignX(b.rng().next()),
				displayAlignY(b.rng().next())});
		e->next = e->current;

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
	p.lifeSpan = 2;              // NORMAL_LIFE + 1 (misc.c:55)
	p.mask = mask;
	p.postProcess = planetPostProcess;
	p.onCollision = solidCollision;
	p.velocity.zero();

	// Mass is assigned only *after* placement (misc.c:71): while the loop runs
	// the planet isn't yet a gravity source, so calculateGravity asks only
	// "is this spot inside someone else's well?" -- which is what rejects it.
	p.mass = 0;

	const EntityId id = b.spawn(Layer::Field, std::move(p));

	do
	{
		auto e = b.get(id);
		e->current = wrap(Vec2i{displayAlignX(b.rng().next()),
				displayAlignY(b.rng().next())});
		e->next = e->current;
	} while (calculateGravity(b, id) || timeSpaceMatterConflict(b, id));

	b.get(id)->mass = b.get(id)->hitPoints;
	return id;
}

EntityId
spawnAsteroid(Battle &b, const CollisionMask *mask)
{
	Element a;
	a.kind = ElementKind::Asteroid;
	a.playerNr = -1;
	a.hitPoints = 1;
	a.mass = 3;
	a.lifeSpan = 1;              // NORMAL_LIFE, and never decremented
	a.mask = mask;
	a.preProcess = asteroidPreProcess;
	a.onCollision = solidCollision;
	a.onDeath = asteroidDeath;

	// Six draws, in the order misc.c:156-191 makes them -- not stylistic:
	// getting the sequence wrong desynchronised network play there, and would
	// desynchronise a replay here.
	const std::uint32_t edge = b.rng().next();
	if ((edge & (1u << 0)) != 0)
	{
		a.current.x = (edge & (1u << 1)) != 0 ? kLogSpaceWidth : 0;
		a.current.y = wrapY(displayAlignY(b.rng().next()));
	}
	else
	{
		a.current.x = wrapX(displayAlignX(b.rng().next()));
		a.current.y = (edge & (1u << 1)) != 0 ? kLogSpaceHeight : 0;
	}

	const std::int32_t magnitude =
			displayToWorld(static_cast<std::int32_t>(b.rng().next() & 7) + 4);
	a.velocity.setVector(magnitude, Facing(static_cast<int>(b.rng().next())));

	a.facing = Facing(static_cast<int>(b.rng().next()));

	// Draws six and seven, in the C's order and truncations (misc.c:156-193):
	// the period, then the direction bit. They used to be bit-packed into
	// Element::thrustWait; the values and their draw order are pinned, the
	// packing is not.
	Spin spin;
	spin.period = static_cast<std::int32_t>(b.rng().next() & 3);
	spin.countdown = spin.period;
	spin.backwards = (b.rng().next() & (1u << 7)) != 0;

	a.next = a.current;
	const EntityId id = b.spawn(Layer::Field, std::move(a));
	b.attach<Spin>(id, spin);
	return id;
}

void
asteroidDeath(Battle &b, EntityId id) noexcept
{
	auto dead = b.get(id);
	if (dead == nullptr)
		return;

	Element r;
	r.kind = ElementKind::Blast;
	r.playerNr = dead->playerNr;
	r.flags = ElementFlags::FiniteLife | ElementFlags::NonSolid;
	r.lifeSpan = 5;
	r.current = dead->current;
	r.next = r.current;
	r.turnWait = 0;
	r.onDeath = rubbleDeath;
	r.mask = dead->mask;

	// Tail insertion: PutElement is PutQueue, which appends at the TAIL
	// (displist.c:142-165). The pre pass's live walk still reaches it this frame.
	(void)b.spawn(Layer::Ordnance, std::move(r));
}

}  // namespace uqm::sim
