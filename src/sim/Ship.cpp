// Copyright the Ur-Quan Masters contributors. GPL-2.0-or-later.

#include "Ship.hpp"

#include "sim/Battle.hpp"
#include "sim/Damage.hpp"
#include "sim/Trig.hpp"

#include <cstddef>
#include <utility>

namespace uqm::sim {

namespace {

// DeltaEnergy: spends or restores, and reports whether it could. Firing is
// gated on this succeeding (ship.c:296-299), so a ship with nine energy and a
// nine-cost weapon fires and empties, and one with eight does not fire at all.
//
// Every success re-arms the regeneration countdown (status.c:317-323) -- and
// that is the mechanic, not bookkeeping: firing pushes regeneration back, so
// a ship that shoots continuously does not regenerate at all while it can
// still afford the shots. Without this an Avenger's flame is close to
// self-sustaining (+4 every few frames against -1 a frame) instead of a
// 16-frame burst. A *failed* spend does not re-arm (the reset sits in the
// C's success branch), so an empty ship's regeneration is not postponed by
// holding the trigger.
bool
deltaEnergy(ShipState &s, std::int32_t delta) noexcept
{
	if (delta < 0 && s.energy + delta < 0)
		return false;

	s.energy += delta;
	if (s.energy > s.spec->maxEnergy)
		s.energy = s.spec->maxEnergy;
	if (s.energy < 0)
		s.energy = 0;
	s.energyCounter = s.spec->energyWait;
	return true;
}

// The silhouette follows the facing. In the C this is implicit -- the
// intersect frame is whatever frame is being displayed (process.c:159-160,
// InitIntersectFrame) -- so a ship that turns is immediately colliding as its
// new shape. Here the coupling is explicit, because the mask lives in the
// simulation and the sprite does not.
void
applyFacingMask(Element &e, const ShipSpec &spec) noexcept
{
	if (spec.facingMasks.empty())
		return;
	const std::size_t i =
			static_cast<std::size_t>(e.facing) % spec.facingMasks.size();
	e.mask = &spec.facingMasks[i];
}

}  // namespace

void
shipPreProcess(Battle &b, EntityId id) noexcept
{
	auto e = b.get(id);
	if (e == nullptr || e->ship.spec == nullptr)
		return;

	ShipState &s = e->ship;
	const ShipSpec &spec = *s.spec;

	if (any(e->flags & ElementFlags::Appearing))
	{
		// First frame: the crew and energy come from the descriptor
		// (ship.c:169). Input is deliberately *not* latched on the appearing
		// frame, so a key held during the countdown does not fire on frame 1.
		s.crew = spec.maxCrew;
		s.energy = spec.maxEnergy;
		s.input = ShipInput::None;
		e->owner = id;  // a ship is its own pParent
		applyFacingMask(*e, spec);
		return;
	}

	// Energy regeneration, gated by its own counter (ship.c:225-230). The
	// counter itself is re-armed inside deltaEnergy on every success, which
	// is how firing postpones regeneration.
	if (s.energyCounter > 0)
	{
		--s.energyCounter;
	}
	else if (s.energy < spec.maxEnergy || spec.energyRegen < 0)
	{
		(void)deltaEnergy(s, spec.energyRegen);
	}

	// The ship's own hook, after regeneration and before turning -- the slot
	// RACE_DESC.preprocess_func occupies (ship.c:232-236). The Ilwrath cloak
	// lives here, which is what makes it win the energy race against the
	// same frame's weapon: the C cloaks first and lets the shot fail.
	if (spec.preProcess != nullptr)
	{
		spec.preProcess(b, id);
		e = b.get(id);
		if (e == nullptr)
			return;
	}

	// Turning. One facing step per turn_wait frames -- ships rotate in whole
	// facings, which is why the trig tables matter (ship.c:238-254).
	if (e->turnWait > 0)
	{
		--e->turnWait;
	}
	else if (any(s.input & (ShipInput::Left | ShipInput::Right)))
	{
		const int delta = any(s.input & ShipInput::Left) ? -1 : 1;
		e->facing = normalizeFacing(e->facing + delta);
		e->turnWait = spec.turnWait;
		applyFacingMask(*e, spec);
	}

	// Thrust (ship.c:256-276). The facing is passed in rather than read off a
	// global -- see Thrust.hpp.
	if (e->thrustWait > 0)
	{
		--e->thrustWait;
	}
	else if (any(s.input & ShipInput::Thrust))
	{
		s.speed = thrust(e->velocity, e->facing, spec.thrust,
				ThrustState{s.speed, s.inGravityWell});
		// ship.c:263-267 clears the whole speed/gravity group and ORs in what
		// inertial_thrust returned. Gravity re-sets the well flag next frame
		// if the ship is still in one, so a ship that leaves the well loses
		// its licence to exceed max speed on the very next thrust.
		s.inGravityWell = false;
		e->thrustWait = spec.thrustWait;

		// Exhaust, only on the frames the ship actually accelerates
		// (ship.c:274) -- not on every frame the key is held.
		//
		// And not while FULLY cloaked: ship.c:271 gates the trail on
		// OBJECT_CLOAKED, which is true only at black -- a half-faded ship
		// still emits one (spawn_ion_trail builds its own POINT_PRIM,
		// tactrans.c:792-832, so the fade of the hull does not dim it).
		if (!any(e->flags & ElementFlags::Cloaked))
			spawnIonTrail(b, id);
	}
}

void
shipPostProcess(Battle &b, EntityId id) noexcept
{
	auto e = b.get(id);
	if (e == nullptr || e->ship.spec == nullptr)
		return;

	ShipState &s = e->ship;
	const ShipSpec &spec = *s.spec;

	// A dead ship does nothing further (ship.c:288-289).
	if (s.crew == 0)
		return;

	// Firing. The energy is spent as part of the test, so a ship that cannot
	// afford the shot does not start the cooldown either.
	if (s.weaponCounter > 0)
	{
		--s.weaponCounter;
	}
	else if (any(s.input & ShipInput::Weapon)
			&& deltaEnergy(s, -spec.weapon.energyCost))
	{
		ShipView view;
		view.position = e->next;
		view.velocity = e->velocity;
		view.facing = e->facing;
		view.playerNr = e->playerNr;
		view.weaponSpeed = spec.weapon.speed;
		view.weaponLife = spec.weapon.life;
		view.weaponDamage = spec.weapon.damage;
		view.weaponHitPoints = spec.weapon.hitPoints;
		view.muzzleOffset = spec.weapon.muzzleOffset;
		view.blastOffset = spec.weapon.blastOffset;

		SpawnBuffer buf{};
		const std::size_t n = spec.weapon.spawn != nullptr
				? spec.weapon.spawn(view, buf)
				: 0;

		for (std::size_t i = 0; i < n; ++i)
		{
			const Spawn &sp = buf[i];
			Element w;
			w.kind = ElementKind::Weapon;
			w.playerNr = sp.playerNr;
			w.current = wrap(sp.position);
			w.next = w.current;
			w.facing = sp.facing;
			w.lifeSpan = sp.life;
			w.hitPoints = sp.hitPoints;
			w.damage = sp.damage;
			// A weapon's mass is its damage (weapon.c:101, and the laser's is
			// 1 at weapon.c:58). Not bookkeeping: CollisionPossible skips any
			// pair where both masses are zero, so a massless shot cannot hit
			// another shot -- which is how a zero here silently turned off
			// flame-intercepts-nuke, the core interaction of this matchup.
			w.mass = sp.damage;
			w.blastOffset = sp.blastOffset;
			// The mask follows the sprite FRAME, not the facing. For the
			// nuke they are the same thing (sixteen facing cels, frameIndex
			// = facing); for the flame they are not (eight ANIMATION cels,
			// frameIndex 0), and indexing by facing made the fireball
			// collide as whichever animation frame the launch facing
			// happened to select. colorCycle carries the frame for the
			// renderer and for any per-frame hook that advances it.
			w.colorCycle = sp.frameIndex;
			w.mask = spec.weapon.masks.empty()
					? nullptr
					: &spec.weapon.masks[static_cast<std::size_t>(
							  sp.frameIndex)
							% spec.weapon.masks.size()];
			w.onCollision = spec.weapon.onCollision != nullptr
					? spec.weapon.onCollision
					: weaponCollision;
			w.preProcess = sp.preProcess != nullptr ? sp.preProcess
													: spec.weapon.preProcess;
			// A guided shot starts with its tracking clock already wound:
			// initialize_nuke seeds turn_wait = TRACK_WAIT (human.c:297-299),
			// so the first steer lands on the fourth hook frame, not the
			// first. Zero for everything unguided.
			w.turnWait = spec.weapon.trackWait;
			// The shot reads its own guidance parameters from the descriptor
			// that fired it, so it carries the pointer. It is not a ship and
			// has no ShipState otherwise.
			w.ship.spec = &spec;
			w.flags = ElementFlags::FiniteLife;
			if (sp.ignoreSimilar)
				w.flags |= ElementFlags::IgnoreSimilar;
			w.velocity.setVector(sp.speed, sp.facing);
			w.owner = id;  // pParent: what IGNORE_SIMILAR is tested against

			// Backed off by one frame of its own muzzle velocity -- the C
			// does this for every missile (weapon.c:126-127). The catch-up
			// pass then integrates it forward, so the first position anyone
			// sees is the muzzle itself, and total travel over its life is
			// life frames of flight, not life + 1.
			{
				const Vec2i v0 = w.velocity.current();
				w.current = wrap(Vec2i{w.current.x - velocityToWorld(v0.x),
						w.current.y - velocityToWorld(v0.y)});
				w.next = w.current;
			}

			if (sp.inheritsVelocity)
			{
				// The ship's velocity is added on top of the muzzle velocity,
				// and the start position is backed off by one frame of it
				// (ilwrath.c:219-222). Without the offset the first frame of
				// flame appears ahead of where the ship actually was.
				const Vec2i v = e->velocity.current();
				w.velocity.deltaComponents(v.x, v.y);
				w.current = wrap(Vec2i{w.current.x - velocityToWorld(v.x),
						w.current.y - velocityToWorld(v.y)});
				w.next = w.current;
			}

			// Tail insertion: a weapon should act *after* the ship that fired
			// it this frame. The step loop's catch-up pass picks it up, so it
			// still moves and can hit on the frame it was fired.
			b.spawnBack(std::move(w));
		}

		s.weaponCounter = spec.weapon.wait;
	}

	// SPECIAL. The engine's own part is only the counter (ship.c:342-343);
	// everything a special *does* is per-ship code, which is the asymmetry
	// that explains most of ships/. Hence a hook rather than a switch.
	//
	// Decrement first, *then* test -- the C decrements and lets the ship see
	// the just-decremented value (ship.c:342-346), so a special can re-fire
	// the frame its counter reaches zero. Gating in an else-branch instead
	// adds a dead frame to every cycle: point defence every 10 frames
	// instead of 9, the cloak re-armed in 14 instead of 13.
	if (s.specialCounter > 0)
		--s.specialCounter;
	if (s.specialCounter == 0 && any(s.input & ShipInput::Special)
			&& spec.special.hook != nullptr)
	{
		spec.special.hook(b, id);
	}
}

int
trackShip(Battle &b, EntityId tracker, int &facing,
		EntityId *outTarget) noexcept
{
	const auto self = b.get(tracker);
	if (self == nullptr)
		return -1;

	// The C reads `next` once the tracker has been preprocessed and `current`
	// before it (weapon.c:356-368), which is the same distinction gravity.c
	// makes -- and for the same reason: half the list has already moved.
	const bool useNext = any(self->flags & ElementFlags::PreProcessed);
	const Vec2i from = useNext ? self->next : self->current;

	int bestDelta = 0;
	std::int32_t bestDistance = 0;
	EntityId bestTarget;
	bool found = false;

	for (EntityId id = b.elements().front(); id.valid();
			id = b.elements().next(id))
	{
		const auto t = b.get(id);
		if (t == nullptr || !any(t->flags & ElementFlags::PlayerShip))
			continue;
		if (t->playerNr == self->playerNr)
			continue;
		// Dead ships are not targets (weapon.c:352-353).
		if (t->lifeSpan == 0 || t->ship.crew == 0)
			continue;
		// Nor cloaked ones (weapon.c:344-348). This is the whole tactical
		// point of the Ilwrath cloak: not that it is hard to see, but that a
		// guided weapon has nothing to steer toward.
		if (any(t->flags & ElementFlags::Cloaked))
			continue;

		const Vec2i to = useNext ? t->next : t->current;
		const Vec2i d = wrapDelta(Vec2i{to.x - from.x, to.y - from.y});
		const int deltaFacing = normalizeFacing(
				angleToFacing(arctan(d.x, d.y)) - facing);

		// Nearest target, by |dx| + |dy| -- the C's own stated approximation
		// of the real distance (weapon.c:378-385). Selecting by *turn* size
		// instead, which is what this did first, makes a missile prefer
		// whatever it happens to be pointed at over whatever is actually
		// close, and that is visible as a nuke that will not commit.
		const std::int32_t adx = d.x < 0 ? -d.x : d.x;
		const std::int32_t ady = d.y < 0 ? -d.y : d.y;
		const std::int32_t distance = adx + ady;

		if (!found || distance < bestDistance)
		{
			bestDistance = distance;
			bestDelta = deltaFacing;
			bestTarget = id;
			found = true;
		}
	}

	// The C's return is best_delta_facing: -1 when nothing was targetable at
	// all, 0 when the chosen target is dead ahead (weapon.c:410-412). The
	// nuke never looks, but the cloak's auto-aim gates on `>= 0`, so folding
	// the two into one value broke the ambush snap and nothing else.
	if (!found)
		return -1;
	if (outTarget != nullptr)
		*outTarget = bestTarget;
	if (bestDelta == 0)
		return 0;

	// weapon.c:398-410. One step per call, and the direction depends on which
	// side of a half-circle the target sits -- with a coin flip when it is
	// exactly astern, because there is no shorter way round and always
	// choosing the same one would make a missile behind you predictable.
	int turn = 0;
	if (bestDelta == kNumFacings / 2)
		turn = static_cast<int>((b.rng().next() & 1u) << 1) - 1;
	else if (bestDelta < kNumFacings / 2)
		turn = 1;
	else
		turn = -1;

	facing = normalizeFacing(facing + turn);
	return bestDelta;
}

void
nukePreProcess(Battle &b, EntityId id) noexcept
{
	auto e = b.get(id);
	if (e == nullptr || e->ship.spec == nullptr)
		return;

	const ShipSpec &spec = *e->ship.spec;

	// Steer, but only every TRACK_WAIT frames (human.c:133-146).
	int facing = e->facing;
	if (e->turnWait > 0)
	{
		--e->turnWait;
	}
	else
	{
		(void)trackShip(b, id, facing);
		e = b.get(id);
		if (e == nullptr)
			return;
		e->facing = facing;
		e->turnWait = spec.weapon.trackWait;

		// And the mask follows the facing. Leaving it at the launch facing is
		// what made a steering nuke look like it was tumbling rather than
		// turning: the sprite changed cel while the rect it was drawn into kept
		// the launch cel's size, so a tall facing got squeezed into a wide one
		// and back. Ships had exactly this defect and it was fixed there.
		// colorCycle is the cel the renderer draws, and for a directional
		// missile that is the facing.
		e->colorCycle = e->facing;
		if (!spec.weapon.masks.empty())
			e->mask = &spec.weapon.masks[static_cast<std::size_t>(e->facing)
					% spec.weapon.masks.size()];
	}

	// And accelerate as it goes (human.c:148-157): speed climbs with how much
	// of its life it has spent, capped. A nuke that has been chasing you for a
	// while is much harder to outrun than one just launched, which is most of
	// what makes the Cruiser feel like the Cruiser.
	std::int32_t speed = spec.weapon.speed
			+ (spec.weapon.life - e->lifeSpan) * spec.weapon.thrustScale;
	if (speed > spec.weapon.maxSpeed)
		speed = spec.weapon.maxSpeed;
	e->velocity.setVector(speed, e->facing);
}

void
flamePreProcess(Battle &b, EntityId id) noexcept
{
	auto e = b.get(id);
	if (e == nullptr)
		return;

	// flame_preprocess (ilwrath.c:126-139): turn_wait and next_turn are both
	// zero, so the fireball's frame advances every frame it lives -- and the
	// collision silhouette follows the animation, which is why the flame
	// GROWS as it flies. CHANGING re-inits the intersect frame in the C
	// (process.c:159-160); here the mask update is that re-init.
	++e->colorCycle;
	const ShipSpec *spec = e->ship.spec;
	if (spec != nullptr && !spec->weapon.masks.empty())
		e->mask = &spec->weapon.masks[static_cast<std::size_t>(e->colorCycle)
				% spec->weapon.masks.size()];
}

void
flameCollision(Battle &b, EntityId id) noexcept
{
	// The C wraps weapon_collision and clears DISAPPEARING
	// (ilwrath.c:141-148): a flame that hit something still burns on screen
	// for the frame it died on, where a spent missile vanishes at once.
	weaponCollision(b, id);
	auto e = b.get(id);
	if (e != nullptr)
		e->flags &= ~ElementFlags::Disappearing;
}

void
spawnIonTrail(Battle &b, EntityId ship) noexcept
{
	const auto e = b.get(ship);
	if (e == nullptr)
		return;

	// Behind the ship, along the reverse of its facing. The C offsets by the
	// sprite's height so the exhaust leaves the hull rather than the hotspot
	// (tactrans.c:808-812); the collision mask stands in for the frame rect.
	const int angle = facingToAngle(e->facing) + kHalfCircle;
	const std::int32_t back = e->mask != nullptr
			? displayToWorld(static_cast<std::int32_t>(e->mask->size().h) / 2)
			: 0;

	Element t;
	t.kind = ElementKind::IonTrail;
	t.playerNr = -1;  // NEUTRAL: exhaust belongs to nobody
	t.flags = ElementFlags::FiniteLife | ElementFlags::NonSolid
			| ElementFlags::IgnoreVelocity;
	t.lifeSpan = kIonTrailLife;
	t.colorCycle = 0;
	t.current = wrap(Vec2i{e->current.x + cosine(angle, back),
			e->current.y + sine(angle, back)});
	t.next = t.current;

	// Head insertion, so exhaust draws behind everything that matters.
	b.spawnFront(std::move(t));
}

void
shipTransition(Battle &b, EntityId id) noexcept
{
	auto e = b.get(id);
	if (e == nullptr || e->ship.spec == nullptr)
		return;

	if (any(e->flags & ElementFlags::Appearing))
	{
		// Arriving: invisible, untouchable, and on a clock
		// (tactrans.c:858-866). The ship is in the simulation the whole time
		// -- it simply cannot be hit and is not drawn -- which is what stops
		// two ships materialising inside each other.
		e->ship.crew = e->ship.spec->maxCrew;
		e->ship.energy = e->ship.spec->maxEnergy;
		e->ship.input = ShipInput::None;
		e->owner = id;
		e->lifeSpan = kWarpInFrames;
		e->flags |= ElementFlags::NonSolid | ElementFlags::FiniteLife;
		e->velocity.zero();
		return;
	}

	// The trail *is* the ship teleporting in.
	//
	// Each frame drops a stationary copy of the hull *behind* the arrival
	// point, offset by TRANSITION_SPEED for every frame still left on the
	// clock (tactrans.c:938-950). The countdown shrinks that offset, so
	// successive images march inward along the facing, the ship itself
	// appears at the end of its own trail, and the trail then fades out
	// behind it.
	//
	// Nothing here moves: the motion is entirely in where each image is put.
	// Two earlier attempts got this wrong in different ways -- first a stack
	// of points at the ship, which is invisible against the hull it sits on,
	// then hull silhouettes flying *outward*, which reads as the ship coming
	// apart rather than arriving. The direction and the shape both carry
	// meaning, and the C encodes both in that one subtraction.
	{
		const int angle = facingToAngle(e->facing);
		const std::int32_t back = kTransitionSpeed * (e->lifeSpan - 1);

		Element shadow;
		shadow.kind = ElementKind::ShipShadow;
		shadow.playerNr = e->playerNr;  // picks which ship's sprites to draw
		shadow.facing = e->facing;
		shadow.mask = e->mask;  // hull-sized, so it is drawn as the hull
		shadow.flags = ElementFlags::FiniteLife | ElementFlags::NonSolid;
		shadow.lifeSpan = kIonTrailLife;
		shadow.current = wrap(Vec2i{e->current.x - cosine(angle, back),
				e->current.y - sine(angle, back)});
		shadow.next = shadow.current;
		shadow.velocity.zero();
		b.spawnFront(std::move(shadow));
	}

	e = b.get(id);
	if (e == nullptr)
		return;

	if (e->lifeSpan <= 1)
	{
		// Arrived. Solid, visible, and under its own control from here
		// (tactrans.c:868-886).
		e->flags &= ~(ElementFlags::NonSolid | ElementFlags::FiniteLife);
		e->velocity.zero();
		e->preProcess = shipPreProcess;
		e->postProcess = shipPostProcess;
		e->lifeSpan = 1;  // NORMAL_LIFE: persistent again
		applyFacingMask(*e, *e->ship.spec);
	}
}

namespace {

// cleanup_dead_ship (tactrans.c:307-337): when the wreck finishes burning,
// everything the dead ship still owns -- in-flight nukes, the flame stream --
// goes with it. The C excepts drifting crew, which does not exist yet.
void
sweepDeadShipOrdnance(Battle &b, EntityId id) noexcept
{
	for (EntityId other = b.elements().front(); other.valid();
			other = b.elements().next(other))
	{
		if (other == id)
			continue;
		auto e = b.get(other);
		if (e == nullptr || !(e->owner == id))
			continue;
		e->lifeSpan = 0;
		e->flags |= ElementFlags::NonSolid | ElementFlags::Disappearing;
	}
}

}  // namespace

void
startShipExplosion(Element &e) noexcept
{
	// The ship becomes its own explosion rather than vanishing
	// (tactrans.c:703-728): it stops dead, loses its energy, stops colliding,
	// and burns for a fixed number of frames. Removing it immediately, which
	// is what happened before, made a kill read as the ship being deleted.
	e.velocity.zero();
	e.ship.energy = 0;
	e.lifeSpan = kExplosionLife;
	e.colorCycle = 0;
	e.flags &= ~ElementFlags::Disappearing;
	e.flags |= ElementFlags::FiniteLife | ElementFlags::NonSolid;
	e.preProcess = explosionPreProcess;
	e.postProcess = nullptr;
	e.onDeath = sweepDeadShipOrdnance;
}

void
explosionPreProcess(Battle &b, EntityId id) noexcept
{
	auto e = b.get(id);
	if (e == nullptr)
		return;

	// How many sparks this frame. The C's schedule (tactrans.c:545-575) ramps
	// up and back down over the 26 frames it spawns for: one at the start,
	// three through the middle, one again at the end, then nothing for the
	// last ten frames while the sparks already thrown finish burning.
	const std::int32_t age = kExplosionLife - e->lifeSpan;
	int count = 3;
	if (age <= 2 || (age >= 20 && age <= 25))
		count = 1;
	else if ((age >= 3 && age <= 5) || age == 18 || age == 19)
		count = 2;
	if (age > 25)
	{
		e->preProcess = nullptr;
		return;
	}

	const Vec2i from = e->current;
	for (int n = 0; n < count; ++n)
	{
		// Scattered around the hull, not on it: a random bearing and a
		// distance of up to eight display pixels, with a third of them thrown
		// eight further out so the cloud has an edge rather than a rim
		// (tactrans.c:597-604).
		const std::uint32_t r0 = b.rng().next();
		const int spot = normalizeAngle(static_cast<int>(r0 >> 16));
		std::int32_t dist = displayToWorld(static_cast<std::int32_t>(r0 % 8u));
		if (((r0 >> 8) & 0xFFu) < 256u / 3u)
			dist += displayToWorld(8);

		// And drifting: its own bearing, unrelated to where it sits, at up to
		// four display pixels a frame. The speed slice is HIBYTE(LOWORD()) --
		// one byte, then the modulo (tactrans.c:607-612); slicing more bits
		// draws a different value from an identical stream, which a replay
		// would notice.
		const std::uint32_t r1 = b.rng().next();
		const int drift = normalizeAngle(static_cast<int>(r1));
		const std::int32_t speed = displayToWorld(
				static_cast<std::int32_t>(((r1 >> 8) & 0xFFu) % 5u));

		Element d;
		d.kind = ElementKind::Debris;
		d.playerNr = -1;  // NEUTRAL: wreckage belongs to nobody
		d.flags = ElementFlags::FiniteLife | ElementFlags::NonSolid;
		d.lifeSpan = kDebrisLife;
		d.current = wrap(Vec2i{from.x + cosine(spot, dist),
				from.y + sine(spot, dist)});
		d.next = d.current;
		// From components at the full 64-angle resolution, the way the C's
		// SetVelocityComponents call is (tactrans.c:607-612). Rounding the
		// drift to 16 facings visibly banded the cloud.
		d.velocity.setComponents(cosine(drift, worldToVelocity(speed)),
				sine(drift, worldToVelocity(speed)));
		b.spawnFront(std::move(d));
	}
}

void
cruiserSpecial(Battle &b, EntityId id) noexcept
{
	auto ship = b.get(id);
	if (ship == nullptr || ship->ship.spec == nullptr)
		return;

	const ShipSpec &spec = *ship->ship.spec;
	const std::int32_t range = spec.special.pointDefenceRange;
	if (range <= 0)
		return;

	const Vec2i from = ship->next;
	bool paid = false;

	// Every shot in range, not just the nearest. The C walks the whole list
	// and fires a laser at each, paying once for the volley (human.c:225-236,
	// the PaidFor flag) -- so a Cruiser surrounded by fire either clears all
	// of it or, if it cannot afford the shot, none of it.
	for (EntityId other = b.elements().front(); other.valid();
			other = b.elements().next(other))
	{
		if (other == id)
			continue;

		auto t = b.get(other);
		if (t == nullptr || !t->collidable())
			continue;
		if (any(t->flags & ElementFlags::Cloaked))
			continue;  // human.c:203-204

		// No ownership test -- the C has none (human.c:203-204), so the
		// Cruiser pays for and shoots down its OWN in-flight nukes within
		// range. That is a real tactical constraint: you cannot hold SPECIAL
		// with a nuke out. An ownership filter here was tried and reverted
		// as an unmarked balance change (review-001 A15).

		// A deliberate divergence: the C filters only on CollidingElement, so
		// it will happily fire the laser at a planet -- which absorbs it,
		// since do_damage exempts gravity masses. Faithful, and it looks like
		// a malfunction. Point defence is for things that can be shot down.
		if (isGravityMass(t->mass))
			continue;

		const Vec2i dv = wrapDelta(
				Vec2i{t->next.x - from.x, t->next.y - from.y});
		const std::int32_t dx = worldToDisplay(dv.x < 0 ? -dv.x : dv.x);
		const std::int32_t dy = worldToDisplay(dv.y < 0 ? -dv.y : dv.y);
		if (dx > range || dy > range || dx * dx + dy * dy > range * range)
			continue;

		if (!paid)
		{
			if (!deltaEnergy(ship->ship, -spec.special.energyCost))
				return;  // cannot afford it, so nothing burns
			ship->ship.specialCounter = spec.special.wait;
			paid = true;
		}

		doDamage(*t, 1);

		// The beam itself. Its only effect on the simulation is the damage
		// above, but the *geometry* lives here rather than in the renderer:
		// that way it is deterministic, it replays identically, and a
		// spectator or a recording draws exactly what happened rather than
		// something reconstructed afterwards.
		//
		// LASER_LIFE is 1 (weapon.c:52). `current` and `next` are its two ends
		// rather than a position and a destination, which is how the C stores
		// a LINE_PRIM too -- and why IgnoreVelocity is set, since those ends
		// must not be integrated as motion.
		const Vec2i beamTo = t->next;
		Element beam;
		beam.kind = ElementKind::Laser;
		beam.playerNr = ship->playerNr;
		beam.owner = id;
		beam.flags = ElementFlags::FiniteLife | ElementFlags::NonSolid
				| ElementFlags::IgnoreVelocity;
		beam.lifeSpan = 1;
		beam.current = from;
		beam.next = beamTo;
		// Tail insertion: the post walk's catch-up reaches it this frame, so
		// its one frame of life is spent -- and drawn -- on the frame it was
		// fired, not the one after.
		b.spawnBack(std::move(beam));

		ship = b.get(id);
		if (ship == nullptr)
			return;
	}
}

// LOOK_AHEAD (ilwrath.c:37): how many frames of both velocities the cloaked
// auto-aim leads the target by.
constexpr int kCloakAimLookAhead = 4;

namespace {

// The ambush snap (ilwrath.c:281-342): firing from full black aims the ship
// at where the nearest enemy will be, not where it is. TrackShip picks the
// target; the facing it steps is thrown away and recomputed from a
// four-frame lead of both ships' velocities.
void
cloakedAutoAim(Battle &b, EntityId id) noexcept
{
	auto e = b.get(id);
	if (e == nullptr)
		return;

	int facing = e->facing;
	EntityId targetId;
	if (trackShip(b, id, facing, &targetId) < 0)
		return;

	auto t = b.get(targetId);
	e = b.get(id);
	if (t == nullptr || e == nullptr)
		return;

	// GetNextVelocityComponents on *copies* (ilwrath.c:292-296): the lead is
	// a question, not a step, and must not disturb either error accumulator.
	Velocity tv = t->velocity;
	Velocity ov = e->velocity;
	const Vec2i dT = tv.advance(kCloakAimLookAhead);
	const Vec2i dO = ov.advance(kCloakAimLookAhead);

	// Raw deltas, no WRAP_DELTA -- the C computes these unwrapped
	// (ilwrath.c:297-300), so an ambush across the seam aims the long way
	// round. Faithful, not an oversight.
	const std::int32_t dx = (t->current.x + dT.x) - (e->current.x + dO.x);
	const std::int32_t dy = (t->current.y + dT.y) - (e->current.y + dO.y);

	e->facing = normalizeFacing(angleToFacing(arctan(dx, dy)));

	// And the ship may not immediately turn away from its own snap
	// (ilwrath.c:335-336).
	if (e->turnWait == 0)
		e->turnWait = 1;

	applyFacingMask(*e, *e->ship.spec);
}

}  // namespace

void
ilwrathPreProcess(Battle &b, EntityId id) noexcept
{
	auto e = b.get(id);
	if (e == nullptr || e->ship.spec == nullptr)
		return;

	ShipState &s = e->ship;
	const ShipSpec &spec = *s.spec;

	// ilwrath_preprocess (ilwrath.c:232-394), with cloakLevel standing in
	// for the prim type and colour. The direction of the walk is derived
	// fresh every frame; there is no stored "cloaking" state to disagree
	// with it.
	//
	// The C masks SPECIAL out of a *local* flags copy when an uncloak step
	// runs (ilwrath.c:346), which suppresses the activation block below for
	// that frame only. A local mirrors that exactly.
	bool specialMasked = false;

	if (s.cloakLevel > 0)  // the prim is STAMPFILL: the machine is engaged
	{
		const bool weaponDischarge = any(s.input & ShipInput::Weapon)
				&& s.energy >= spec.weapon.energyCost;

		if (weaponDischarge
				|| (s.specialCounter == 0
						&& (any(s.input & ShipInput::Special)
								|| s.cloakLevel < kCloakFullLevel)))
		{
			// One step toward visible (ilwrath.c:250-348). Firing is the
			// only trigger that works mid-debounce; a key press needs the
			// counter spent, and "not yet black with a spent counter" keeps
			// an interrupted ramp unwinding all the way out on its own.
			if (s.cloakLevel == kCloakFullLevel && weaponDischarge)
			{
				// Stepping off BLACK under fire is the ambush.
				cloakedAutoAim(b, id);
				e = b.get(id);
				if (e == nullptr)
					return;
			}
			--s.cloakLevel;  // reaching 0 is the C's SetPrimType(STAMP)

			// Every uncloak step zeroes the debounce (ilwrath.c:347), so
			// re-cloak is available the moment the ship is solid again --
			// and masks SPECIAL for this frame, so the same press cannot
			// also activate below.
			s.specialCounter = 0;
			specialMasked = true;
		}
		else if (s.cloakLevel < kCloakFullLevel)
		{
			// One step toward black (ilwrath.c:349-374). At black, nothing:
			// the ship stays hidden until something above fires.
			++s.cloakLevel;
		}
	}

	// OBJECT_CLOAKED is STAMPFILL *and* BLACK (element.h:201-204): hidden
	// from tracking and point defence only when fully faded, in either
	// direction of the walk.
	if (s.cloakLevel == kCloakFullLevel)
		e->flags |= ElementFlags::Cloaked;
	else
		e->flags &= ~ElementFlags::Cloaked;

	// Activation (ilwrath.c:377-393): SPECIAL with the debounce spent, and
	// the energy is paid again every time -- there is no free toggle-off and
	// no half-price re-cloak. The ramp restarts at white even from mid-fade,
	// though in practice it can only fire from solid: any earlier level
	// either has the counter running or just masked SPECIAL above.
	if (!specialMasked && any(s.input & ShipInput::Special)
			&& s.specialCounter == 0
			&& deltaEnergy(s, -spec.special.energyCost))
	{
		s.cloakLevel = 1;  // WHITE, the walk's first colour
		s.specialCounter = spec.special.wait;
	}
}

// --------------------------------------------------------------------------
// The two M1 ships.

const ShipSpec &
earthlingCruiser() noexcept
{
	// human.c:27-49. MAX_THRUST 24, THRUST_INCREMENT 3.
	static const ShipSpec data = [] {
		ShipSpec d;
		d.maxCrew = 18;
		d.maxEnergy = 18;
		d.energyRegen = 1;
		d.energyWait = 8;
		d.thrust = ThrustProfile{24, 3};
		d.thrustWait = 4;
		d.turnWait = 1;
		d.weapon.wait = 10;
		d.weapon.energyCost = 9;
		d.special.wait = 9;
		d.special.energyCost = 4;
		d.mass = 6;
		d.weapon.spawn = spawnCruiserPrimary;
		// MISSILE_SPEED is max(MAX_THRUST, DISPLAY_TO_WORLD(10)) == 40.
		d.weapon.speed = 40;
		d.weapon.life = 60;
		d.weapon.damage = 4;
		d.weapon.hitPoints = 1;
		d.weapon.muzzleOffset = 42;   // HUMAN_OFFSET
		d.weapon.blastOffset = 8;     // NUKE_OFFSET

		// The nuke is guided and accelerates: TRACK_WAIT 3,
		// MAX_MISSILE_SPEED = DISPLAY_TO_WORLD(20) == 80, THRUST_SCALE =
		// DISPLAY_TO_WORLD(1) == 4 (human.c:43-50).
		d.weapon.trackWait = 3;
		d.weapon.maxSpeed = 80;
		d.weapon.thrustScale = 4;
		d.weapon.preProcess = nukePreProcess;
		d.special.hook = cruiserSpecial;
		d.special.pointDefenceRange = 100;   // LASER_RANGE
		return d;
	}();
	return data;
}

const ShipSpec &
ilwrathAvenger() noexcept
{
	// ilwrath.c:28-49. Note THRUST_WAIT 0 and WEAPON_WAIT 0 -- the Avenger
	// accelerates every frame and its flame is continuous, which is why it
	// feels nothing like the Cruiser.
	static const ShipSpec data = [] {
		ShipSpec d;
		d.maxCrew = 22;
		d.maxEnergy = 16;
		d.energyRegen = 4;
		d.energyWait = 4;
		d.thrust = ThrustProfile{25, 5};
		d.thrustWait = 0;
		d.turnWait = 2;
		d.weapon.wait = 0;
		d.weapon.energyCost = 1;
		d.special.wait = 13;
		d.special.energyCost = 3;
		d.mass = 7;
		d.weapon.spawn = spawnAvengerPrimary;
		d.weapon.speed = 25;    // MISSILE_SPEED == MAX_THRUST
		d.weapon.life = 8;
		d.weapon.damage = 1;
		d.weapon.hitPoints = 1;
		d.weapon.muzzleOffset = 29;   // ILWRATH_OFFSET
		d.weapon.blastOffset = 0;     // MISSILE_OFFSET
		d.weapon.preProcess = flamePreProcess;
		d.weapon.onCollision = flameCollision;
		// The cloak is the ship hook, not a post-phase special: it must win
		// the energy race against the same frame's shot (see ShipSpec).
		d.preProcess = ilwrathPreProcess;
		return d;
	}();
	return data;
}

}  // namespace uqm::sim
