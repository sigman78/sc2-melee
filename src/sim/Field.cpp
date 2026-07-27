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

// DISPLAY_ALIGN_X/Y (units.h:85-86). The random word is first truncated to 16
// bits -- COUNT is an unsigned short -- then folded into the arena and snapped
// down to a display pixel. The truncation is why an asteroid never appears in
// the far reaches of an 8192-wide arena from a 31-bit draw: it only ever sees
// the low half.
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

// asteroid_preprocess (misc.c:107-128). An asteroid does nothing but tumble.
// `thrustWait` is not a thrust delay here: bit 7 is the spin direction and the
// low seven bits are the period. The C keeps the rotation in the sprite's
// frame index; there is no sprite in the simulation, so it lives in `facing`,
// which is what a facing means for something with no heading.
void
asteroidPreProcess(Battle &b, EntityId id) noexcept
{
	Element *e = b.get(id);
	if (e == nullptr)
		return;

	if (e->turnWait > 0)
	{
		--e->turnWait;
		return;
	}

	const bool backwards = (e->thrustWait & (1 << 7)) != 0;
	e->facing = normalizeFacing(e->facing + (backwards ? -1 : 1));
	e->turnWait = e->thrustWait & ((1 << 7) - 1);
}

// The rubble's own death hook: put a fresh asteroid back into the field.
void
rubbleDeath(Battle &b, EntityId id) noexcept
{
	const Element *e = b.get(id);
	const CollisionMask *mask = (e != nullptr) ? e->mask : nullptr;
	(void)spawnAsteroid(b, mask);
}

}  // namespace

bool
timeSpaceMatterConflict(Battle &b, EntityId id)
{
	const Element *self = b.get(id);
	if (self == nullptr || self->mask == nullptr)
		return false;

	const Body a{self->mask, self->current, self->current};

	for (EntityId other = b.elements().front(); other.valid();
			other = b.elements().next(other))
	{
		if (other == id)
			continue;

		const Element *t = b.get(other);
		if (t == nullptr || t->mask == nullptr)
			continue;

		// A player ship counts even when it is not collidable -- gravity.c:175
		// calls that case "ship in transition", and it is what stops a planet
		// materialising on top of a ship that is still warping in.
		if (!t->collidable() && !any(t->flags & ElementFlags::PlayerShip))
			continue;

		const Body other_{t->mask, t->current, t->current};
		if (sweptIntersect(a, other_))
			return true;
	}

	return false;
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

	// Mass is assigned only *after* placement in the C (misc.c:71), and that
	// ordering is the point: while the loop runs the planet is not yet a
	// gravity source, so calculateGravity asks the other question -- "is this
	// spot inside someone else's well?" -- which is what rejects it.
	p.mass = 0;

	const EntityId id = b.spawnBack(std::move(p));

	do
	{
		Element *e = b.get(id);
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

	// Six draws, in the order misc.c:156-191 makes them. The comment there
	// about temporaries and argument evaluation order is not stylistic --
	// getting this sequence wrong desynchronises a network game, and here it
	// would desynchronise a replay.
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
	const int facing = static_cast<int>(b.rng().next());
	a.velocity.setVector(magnitude, facing);

	a.facing = normalizeFacing(static_cast<int>(b.rng().next()));
	a.thrustWait = static_cast<std::int32_t>(b.rng().next() & 3);
	a.turnWait = a.thrustWait;

	a.next = a.current;
	return b.spawnBack(std::move(a));
}

void
asteroidDeath(Battle &b, EntityId id) noexcept
{
	const Element *dead = b.get(id);
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

	// Head insertion, because spawn_rubble calls PutElement *before* filling
	// the element in (misc.c:90) -- the rubble is in the list ahead of the
	// asteroid that is dying, and gets its catch-up preprocess this frame.
	(void)b.spawnFront(std::move(r));
}

}  // namespace uqm::sim
