// Copyright the Ur-Quan Masters contributors. GPL-2.0-or-later.

#include "Ship.hpp"

#include "engine/core/Types.hpp"
#include "sim/Battle.hpp"
#include "sim/Damage.hpp"
#include "sim/Trig.hpp"

#include <utility>

namespace uqm::sim {

namespace {

// Spends or restores energy, reporting whether it could; firing gates on
// success (ship.c:296-299). Every success re-arms the regen countdown
// (status.c:317-323); a failed spend does not.
bool
deltaEnergy(ShipState &s, i32 delta) noexcept
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

// The silhouette follows the facing: implicit in the C (process.c:159-160,
// InitIntersectFrame reads whatever frame is displayed); explicit here,
// since the mask lives in the simulation and there's no sprite.
void
applyFacingMask(Element &e, const ShipSpec &spec) noexcept
{
	if (spec.facingMasks.empty())
		return;
	const usize i =
			static_cast<usize>(e.facing.raw()) % spec.facingMasks.size();
	e.mask = &spec.facingMasks[i];
}

// Energy regeneration, gated by its own counter (ship.c:225-230). The
// counter itself is re-armed inside deltaEnergy on every success, which is
// how firing postpones regeneration.
void
regenEnergy(ShipState &s, const ShipSpec &spec) noexcept
{
	if (s.energyCounter > 0)
	{
		--s.energyCounter;
	}
	else if (s.energy < spec.maxEnergy || spec.energyRegen < 0)
	{
		(void)deltaEnergy(s, spec.energyRegen);
	}
}

// Turning. One facing step per turn_wait frames -- ships rotate in whole
// facings, which is why the trig tables matter (ship.c:238-254).
void
turnShip(Battle &b, EntityId id, Element &e, ShipState &,
		const ShipSpec &spec) noexcept
{
	const Input &in = *b.find<Input>(id);
	if (e.turnWait > 0)
	{
		--e.turnWait;
	}
	else if (any(in.buttons & (ShipInput::Left | ShipInput::Right)))
	{
		const int delta = any(in.buttons & ShipInput::Left) ? -1 : 1;
		e.facing += delta;
		e.turnWait = spec.turnWait;
		applyFacingMask(e, spec);
	}
}

// Thrust (ship.c:256-276). The facing is passed in rather than read off a
// global -- see Thrust.hpp.
void
applyThrustInput(Battle &b, EntityId id, Element &e, ShipState &s,
		const ShipSpec &spec) noexcept
{
	const Input &in = *b.find<Input>(id);
	if (e.thrustWait > 0)
	{
		--e.thrustWait;
	}
	else if (any(in.buttons & ShipInput::Thrust))
	{
		s.speed = thrust(e.velocity, e.facing, spec.thrust,
				ThrustState{s.speed, s.inGravityWell});
		// ship.c:263-267 clears the whole speed/gravity group and ORs in the
		// thrust result; gravity re-sets the well flag next frame if still inside
		// one, so leaving the well loses the licence to exceed max speed at once.
		s.inGravityWell = false;
		e.thrustWait = spec.thrustWait;

		// Exhaust only on frames the ship actually accelerates (ship.c:274), and
		// not while FULLY cloaked (ship.c:271 gates on OBJECT_CLOAKED, true only at
		// black) -- a half-faded ship still emits one (tactrans.c:792-832).
		if (!isCloaked(b, id))
			spawnIonTrail(b, id);
	}
}

// Firing. The energy is spent as part of the test, so a ship that cannot
// afford the shot does not start the cooldown either.
void
fireWeapon(Battle &b, EntityId id, Element &e, ShipState &s,
		const ShipSpec &spec) noexcept
{
	const Input &in = *b.find<Input>(id);
	if (s.weaponCounter > 0)
	{
		--s.weaponCounter;
	}
	else if (any(in.buttons & ShipInput::Weapon)
			&& deltaEnergy(s, -spec.weapon.energyCost))
	{
		ShipView view;
		view.position = e.next;
		view.velocity = e.velocity;
		view.facing = e.facing;
		view.playerNr = e.playerNr;
		view.weaponSpeed = spec.weapon.speed;
		view.weaponLife = spec.weapon.life;
		view.weaponDamage = spec.weapon.damage;
		view.weaponHitPoints = spec.weapon.hitPoints;
		view.muzzleOffset = spec.weapon.muzzleOffset;
		view.blastOffset = spec.weapon.blastOffset;

		SpawnBuffer buf{};
		const usize n = spec.weapon.spawn != nullptr
				? spec.weapon.spawn(view, buf)
				: 0;

		for (usize i = 0; i < n; ++i)
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
			// A weapon's mass is its damage (weapon.c:101; laser's is 1, weapon.c:58).
			// Not bookkeeping: CollisionPossible skips pairs where both masses are
			// zero, so a massless shot can't hit another shot.
			w.mass = sp.damage;
			w.blastOffset = sp.blastOffset;
			// The mask follows the sprite FRAME, not the facing: same thing for the
			// nuke (16 facing cels, frameIndex = facing), different for the flame (8
			// ANIMATION cels, frameIndex 0). colorCycle carries the frame.
			w.colorCycle = sp.frameIndex;
			w.mask = spec.weapon.masks.empty()
					? nullptr
					: &spec.weapon.masks[static_cast<usize>(sp.frameIndex)
							% spec.weapon.masks.size()];
			w.onCollision = spec.weapon.onCollision != nullptr
					? spec.weapon.onCollision
					: weaponCollision;
			w.preProcess = sp.preProcess != nullptr ? sp.preProcess
													: spec.weapon.preProcess;
			w.flags = ElementFlags::FiniteLife;
			if (sp.ignoreSimilar)
				w.flags |= ElementFlags::IgnoreSimilar;
			w.velocity.setVector(sp.speed, sp.facing);
			w.owner = id;  // pParent: what IGNORE_SIMILAR is tested against

			// Backed off by one frame of its own muzzle velocity, as the C does for
			// every missile (weapon.c:126-127); the catch-up pass integrates it
			// forward, so total travel over its life is life frames, not life + 1.
			{
				const Vec2i v0 = w.velocity.current();
				w.current = wrap(Vec2i{w.current.x - velocityToWorld(v0.x),
						w.current.y - velocityToWorld(v0.y)});
				w.next = w.current;
			}

			if (sp.inheritsVelocity)
			{
				// The ship's velocity adds on top of the muzzle velocity, and the start
				// position backs off by one frame of it (ilwrath.c:219-222) -- otherwise
				// the first frame of flame appears ahead of where the ship actually was.
				const Vec2i v = e.velocity.current();
				w.velocity.deltaComponents(v.x, v.y);
				w.current = wrap(Vec2i{w.current.x - velocityToWorld(v.x),
						w.current.y - velocityToWorld(v.y)});
				w.next = w.current;
			}

			// Tail insertion: a weapon should act *after* the ship that fired
			// it this frame. The step loop's catch-up pass picks it up, so it
			// still moves and can hit on the frame it was fired.
			const EntityId wid = b.spawn(Layer::Ordnance, std::move(w));
			b.attachWeaponSpec(wid, &spec.weapon);

			// A guided shot starts with its tracking clock already wound:
			// initialize_nuke seeds TRACK_WAIT (human.c:297-299), so the
			// first steer lands on the fourth hook frame. The spec declares
			// guidance by having any of the numbers; the component carries
			// them plus the shot's own clock.
			if (spec.weapon.trackWait > 0 || spec.weapon.thrustScale > 0)
			{
				b.attach<Guided>(wid,
						Guided{spec.weapon.trackWait, spec.weapon.maxSpeed,
							spec.weapon.thrustScale, spec.weapon.trackWait});
			}
		}

		s.weaponCounter = spec.weapon.wait;
	}
}

// SPECIAL: the engine only ticks the counter (ship.c:342-343). Decrement
// first, then test the just-decremented value (ship.c:342-346) -- gating
// in an else-branch instead adds a dead frame every cycle.
void
gateSpecial(Battle &b, EntityId id, ShipState &s,
		const ShipSpec &spec) noexcept
{
	const Input &in = *b.find<Input>(id);
	if (s.specialCounter > 0)
		--s.specialCounter;
	if (s.specialCounter == 0 && any(in.buttons & ShipInput::Special)
			&& spec.special.hook != nullptr)
	{
		spec.special.hook(b, id);
	}
}

void warpInStep(Battle &b, EntityId id) noexcept;
void explosionStep(Battle &b, EntityId id) noexcept;

}  // namespace

void
shipPreProcess(Battle &b, EntityId id) noexcept
{
	auto e = b.get(id);
	if (e == nullptr)
		return;
	ShipState *sp = b.ship(id);
	if (sp == nullptr)
		return;

	ShipState &s = *sp;
	const ShipSpec &spec = *s.spec;
	Input &in = *b.find<Input>(id);

	if (b.has<WarpingIn>(id))
	{
		warpInStep(b, id);
		return;
	}

	if (any(e->flags & ElementFlags::Appearing))
	{
		// First frame: the crew and energy come from the descriptor
		// (ship.c:169). Input is deliberately *not* latched on the appearing
		// frame, so a key held during the countdown does not fire on frame 1.
		s.crew = spec.maxCrew;
		s.energy = spec.maxEnergy;
		in.buttons = ShipInput::None;
		e->owner = id;  // a ship is its own pParent
		applyFacingMask(*e, spec);
		return;
	}

	// A dead hull runs its explosion and nothing else (tactrans.c:703-728).
	if (s.crew == 0)
	{
		if (b.has<Exploding>(id))
			explosionStep(b, id);
		return;
	}

	regenEnergy(s, spec);

	// The ship's own hook, after regen and before turning -- RACE_DESC
	// .preprocess_func's slot (ship.c:232-236). The Ilwrath cloak lives here,
	// winning the energy race against the same frame's weapon.
	if (spec.preProcess != nullptr)
	{
		spec.preProcess(b, id);
		e = b.get(id);
		if (e == nullptr)
			return;
	}

	turnShip(b, id, *e, s, spec);
	applyThrustInput(b, id, *e, s, spec);
}

void
shipPostProcess(Battle &b, EntityId id) noexcept
{
	auto e = b.get(id);
	if (e == nullptr)
		return;
	ShipState *sp = b.ship(id);
	if (sp == nullptr)
		return;

	ShipState &s = *sp;
	const ShipSpec &spec = *s.spec;

	if (b.has<WarpingIn>(id))
		return;

	// A dead ship does nothing further (ship.c:288-289).
	if (s.crew == 0)
		return;

	fireWeapon(b, id, *e, s, spec);
	gateSpecial(b, id, s, spec);
}

int
trackShip(Battle &b, EntityId tracker, Facing &facing,
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
	i32 bestDistance = 0;
	EntityId bestTarget;
	bool found = false;

	for (EntityId id = b.front(); id != kNoEntity;
			id = b.next(id))
	{
		const auto t = b.get(id);
		if (t == nullptr || !b.has<PlayerShip>(id))
			continue;
		if (t->playerNr == self->playerNr)
			continue;
		// Dead ships are not targets (weapon.c:352-353).
		const ShipState *ts = b.ship(id);
		if (t->lifeSpan == 0 || ts == nullptr || ts->crew == 0)
			continue;
		// Nor cloaked ones (weapon.c:344-348). This is the whole tactical
		// point of the Ilwrath cloak: not that it is hard to see, but that a
		// guided weapon has nothing to steer toward.
		if (isCloaked(b, id))
			continue;

		const Vec2i to = useNext ? t->next : t->current;
		const Vec2i d = wrapDelta(Vec2i{to.x - from.x, to.y - from.y});
		const int deltaFacing = Angle(arctan(d.x, d.y)).facing() - facing;

		// Nearest target, by |dx| + |dy| -- the C's own stated approximation of
		// the real distance (weapon.c:378-385).
		const i32 adx = d.x < 0 ? -d.x : d.x;
		const i32 ady = d.y < 0 ? -d.y : d.y;
		const i32 distance = adx + ady;

		if (!found || distance < bestDistance)
		{
			bestDistance = distance;
			bestDelta = deltaFacing;
			bestTarget = id;
			found = true;
		}
	}

	// The C's return is best_delta_facing: -1 when nothing was targetable,
	// 0 when dead ahead (weapon.c:410-412). The nuke never looks, but the
	// cloak's auto-aim gates on `>= 0`.
	if (!found)
		return -1;
	if (outTarget != nullptr)
		*outTarget = bestTarget;
	if (bestDelta == 0)
		return 0;

	// weapon.c:398-410: one step per call, direction by which side of a
	// half-circle the target sits -- a coin flip exactly astern, since always
	// choosing the same side would make a missile behind you predictable.
	int turn = 0;
	if (bestDelta == kNumFacings / 2)
		turn = static_cast<int>((b.rng().next() & 1u) << 1) - 1;
	else if (bestDelta < kNumFacings / 2)
		turn = 1;
	else
		turn = -1;

	facing += turn;
	return bestDelta;
}

void
nukePreProcess(Battle &b, EntityId id) noexcept
{
	auto e = b.get(id);
	if (e == nullptr)
		return;
	Borrowed<const WeaponSpec> ws = b.weaponSpec(id);
	Guided *g = b.find<Guided>(id);
	if (ws == nullptr || g == nullptr)
		return;

	// Steer, but only every TRACK_WAIT frames (human.c:133-146). The clock
	// is the component's own, not a repurposed Element::turnWait.
	Facing facing = e->facing;
	if (g->clock > 0)
	{
		--g->clock;
	}
	else
	{
		(void)trackShip(b, id, facing);
		e = b.get(id);
		if (e == nullptr)
			return;
		e->facing = facing;
		g->clock = g->trackWait;

		// The mask follows the facing too, or a steering nuke's rect keeps the
		// launch cel's size while the sprite changes cel. colorCycle is the cel
		// the renderer draws.
		e->colorCycle = e->facing.raw();
		if (!ws->masks.empty())
			e->mask = &ws->masks[static_cast<usize>(e->facing.raw())
					% ws->masks.size()];
	}

	// Accelerates as it goes (human.c:148-157): speed climbs with life spent,
	// capped -- a nuke chasing you a while is much harder to outrun than one
	// just launched.
	i32 speed = ws->speed + (ws->life - e->lifeSpan) * g->thrustScale;
	if (speed > g->maxSpeed)
		speed = g->maxSpeed;
	e->velocity.setVector(speed, e->facing);
}

void
flamePreProcess(Battle &b, EntityId id) noexcept
{
	auto e = b.get(id);
	if (e == nullptr)
		return;

	// flame_preprocess (ilwrath.c:126-139): the frame advances every frame it
	// lives, and the collision silhouette follows -- why the flame GROWS as it
	// flies (mask update = the C's CHANGING re-init, process.c:159-160).
	++e->colorCycle;
	Borrowed<const WeaponSpec> ws = b.weaponSpec(id);
	if (ws != nullptr && !ws->masks.empty())
		e->mask = &ws->masks[static_cast<usize>(e->colorCycle)
				% ws->masks.size()];
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
	const Angle angle = e->facing.angle().opposite();
	const i32 back = e->mask != nullptr
			? displayToWorld(static_cast<i32>(e->mask->size().h) / 2)
			: 0;

	Element t;
	t.kind = ElementKind::IonTrail;
	t.playerNr = -1;  // NEUTRAL: exhaust belongs to nobody
	t.flags = ElementFlags::FiniteLife | ElementFlags::NonSolid;
	t.lifeSpan = kIonTrailLife;
	t.colorCycle = 0;
	t.current = wrap(Vec2i{e->current.x + cosine(angle, back),
			e->current.y + sine(angle, back)});
	t.next = t.current;

	// Head insertion, so exhaust draws behind everything that matters.
	// Tags attach after the spawn hands out the id; the walk reaches the
	// trail later than this statement, so it never sees a half-built one.
	const EntityId trail = b.spawn(Layer::Background, std::move(t));
	b.attach<IgnoreVelocity>(trail);
}

namespace {

void
warpInStep(Battle &b, EntityId id) noexcept
{
	auto e = b.get(id);
	if (e == nullptr)
		return;
	ShipState *sp = b.ship(id);
	if (sp == nullptr)
		return;

	if (any(e->flags & ElementFlags::Appearing))
	{
		// Arriving: invisible, untouchable, on a clock (tactrans.c:858-866). The
		// ship is in the simulation the whole time -- just not hittable or drawn --
		// which stops two ships materialising inside each other.
		sp->crew = sp->spec->maxCrew;
		sp->energy = sp->spec->maxEnergy;
		b.find<Input>(id)->buttons = ShipInput::None;
		e->owner = id;
		e->lifeSpan = kWarpInFrames;
		e->flags |= ElementFlags::NonSolid | ElementFlags::FiniteLife;
		e->velocity.zero();
		return;
	}

	// The trail *is* the ship teleporting in: each frame drops a stationary
	// hull copy behind the arrival point, shrinking by TRANSITION_SPEED per
	// frame left (tactrans.c:938-950), so images march inward to the ship.
	{
		const Angle angle = e->facing.angle();
		const i32 back = kTransitionSpeed * (e->lifeSpan - 1);

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
		b.spawn(Layer::Background, std::move(shadow));
	}

	e = b.get(id);
	if (e == nullptr)
		return;

	if (e->lifeSpan <= 1)
	{
		// Arrived: solid, visible, under its own control (tactrans.c:868-886).
		e->flags &= ~(ElementFlags::NonSolid | ElementFlags::FiniteLife);
		e->velocity.zero();
		e->lifeSpan = 1;  // NORMAL_LIFE: persistent again
		applyFacingMask(*e, *sp->spec);
		b.detach<WarpingIn>(id);
	}
}

// cleanup_dead_ship (tactrans.c:307-337): when the wreck finishes burning,
// everything the dead ship still owns -- in-flight nukes, the flame stream --
// goes with it. The C excepts drifting crew, which does not exist yet.
void
sweepDeadShipOrdnance(Battle &b, EntityId id) noexcept
{
	for (EntityId other = b.front(); other != kNoEntity;
			other = b.next(other))
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
startShipExplosion(Battle &b, EntityId id) noexcept
{
	auto e = b.get(id);
	if (e == nullptr)
		return;

	// The ship becomes its own explosion rather than vanishing
	// (tactrans.c:703-728): stops dead, loses energy, stops colliding, and
	// burns for a fixed number of frames.
	e->velocity.zero();
	if (ShipState *sp = b.ship(id))
		sp->energy = 0;
	e->lifeSpan = kExplosionLife;
	e->colorCycle = 0;
	e->flags &= ~ElementFlags::Disappearing;
	e->flags |= ElementFlags::FiniteLife | ElementFlags::NonSolid;
	e->postProcess = nullptr;
	e->onDeath = sweepDeadShipOrdnance;
	if (!b.has<Exploding>(id))
		b.attach<Exploding>(id);
}

namespace {

void
explosionStep(Battle &b, EntityId id) noexcept
{
	auto e = b.get(id);
	if (e == nullptr)
		return;

	// How many sparks this frame: the C's schedule (tactrans.c:545-575) ramps
	// 1/3/1 over the 26 frames it spawns for, then nothing for the last ten
	// while thrown sparks finish burning.
	const i32 age = kExplosionLife - e->lifeSpan;
	int count = 3;
	if (age <= 2 || (age >= 20 && age <= 25))
		count = 1;
	else if ((age >= 3 && age <= 5) || age == 18 || age == 19)
		count = 2;
	if (age > 25)
	{
		b.detach<Exploding>(id);
		return;
	}

	const Vec2i from = e->current;
	for (int n = 0; n < count; ++n)
	{
		// Scattered around the hull: random bearing, up to 8 display pixels out, a
		// third thrown 8 further so the cloud has an edge, not a rim
		// (tactrans.c:597-604).
		const u32 r0 = b.rng().next();
		const Angle spot{static_cast<int>(r0 >> 16)};
		i32 dist = displayToWorld(static_cast<i32>(r0 % 8u));
		if (((r0 >> 8) & 0xFFu) < 256u / 3u)
			dist += displayToWorld(8);

		// Drifting: its own bearing, up to 4 display pixels a frame. The speed
		// slice is HIBYTE(LOWORD()) -- one byte, then modulo (tactrans.c:607-612);
		// slicing different bits draws a different value from the same stream.
		const u32 r1 = b.rng().next();
		const Angle drift{static_cast<int>(r1)};
		const i32 speed = displayToWorld(
				static_cast<i32>(((r1 >> 8) & 0xFFu) % 5u));

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
		b.spawn(Layer::Background, std::move(d));
	}
}

}  // namespace

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

// LOOK_AHEAD (ilwrath.c:37): how many frames of both velocities the cloaked
// auto-aim leads the target by.
constexpr int kCloakAimLookAhead = 4;

namespace {

// The ambush snap (ilwrath.c:281-342): firing from full black aims at
// where the nearest enemy will be. TrackShip picks the target; the facing
// it steps is thrown away and recomputed from a four-frame lead.
void
cloakedAutoAim(Battle &b, EntityId id) noexcept
{
	auto e = b.get(id);
	if (e == nullptr)
		return;

	Facing facing = e->facing;
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
	const i32 dx = (t->current.x + dT.x) - (e->current.x + dO.x);
	const i32 dy = (t->current.y + dT.y) - (e->current.y + dO.y);

	e->facing = Angle(arctan(dx, dy)).facing();

	// And the ship may not immediately turn away from its own snap
	// (ilwrath.c:335-336).
	if (e->turnWait == 0)
		e->turnWait = 1;

	applyFacingMask(*e, *b.ship(id)->spec);
}

}  // namespace

void
ilwrathPreProcess(Battle &b, EntityId id) noexcept
{
	auto e = b.get(id);
	if (e == nullptr)
		return;
	ShipState *sp = b.ship(id);
	if (sp == nullptr)
		return;

	ShipState &s = *sp;
	const ShipSpec &spec = *s.spec;
	const Input &in = *b.find<Input>(id);

	// ilwrath_preprocess (ilwrath.c:232-394): the Cloak component's level
	// stands in for the prim type/colour, its walk direction derived fresh
	// each frame -- no stored "cloaking" state to disagree with it, and
	// OBJECT_CLOAKED is isCloaked(), derived from the same level. The
	// machine owns its component, so only ships that run this hook ever
	// carry one.
	Cloak *c = b.find<Cloak>(id);
	if (c == nullptr)
		c = &b.attach<Cloak>(id);

	// The C masks SPECIAL out of a *local* flags copy when an uncloak step
	// runs (ilwrath.c:346), which suppresses the activation block below for
	// that frame only. A local mirrors that exactly.
	bool specialMasked = false;

	if (c->level > 0)  // the prim is STAMPFILL: the machine is engaged
	{
		const bool weaponDischarge = any(in.buttons & ShipInput::Weapon)
				&& s.energy >= spec.weapon.energyCost;

		if (weaponDischarge
				|| (s.specialCounter == 0
						&& (any(in.buttons & ShipInput::Special)
								|| c->level < kCloakFullLevel)))
		{
			// One step toward visible (ilwrath.c:250-348). Firing is the only trigger
			// that works mid-debounce; a key press needs the counter spent, so an
			// interrupted ramp keeps unwinding out on its own.
			if (c->level == kCloakFullLevel && weaponDischarge)
			{
				// Stepping off BLACK under fire is the ambush.
				cloakedAutoAim(b, id);
				e = b.get(id);
				if (e == nullptr)
					return;
			}
			--c->level;  // reaching 0 is the C's SetPrimType(STAMP)

			// Every uncloak step zeroes the debounce (ilwrath.c:347): re-cloak is
			// available the moment the ship is solid again, and SPECIAL is masked
			// this frame so the same press can't also activate below.
			s.specialCounter = 0;
			specialMasked = true;
		}
		else if (c->level < kCloakFullLevel)
		{
			// One step toward black (ilwrath.c:349-374). At black, nothing:
			// the ship stays hidden until something above fires.
			++c->level;
		}
	}

	// Activation (ilwrath.c:377-393): SPECIAL with the debounce spent, paying
	// energy every time -- no free toggle-off, no half-price re-cloak. Restarts
	// at white even from mid-fade, though only reachable from solid in practice.
	if (!specialMasked && any(in.buttons & ShipInput::Special)
			&& s.specialCounter == 0
			&& deltaEnergy(s, -spec.special.energyCost))
	{
		c->level = 1;  // WHITE, the walk's first colour
		s.specialCounter = spec.special.wait;
	}
}

bool
isCloaked(const Battle &b, EntityId id) noexcept
{
	// OBJECT_CLOAKED is STAMPFILL *and* BLACK (element.h:201-204): hidden
	// only when fully faded, in either direction of the walk.
	const Cloak *c = b.find<const Cloak>(id);
	return c != nullptr && c->level == kCloakFullLevel;
}

// --------------------------------------------------------------------------
// The two M1 ships.

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
			.preProcess = nukePreProcess,
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

const ShipSpec &
ilwrathAvenger() noexcept
{
	// ilwrath.c:27-53. THRUST_WAIT 0 and WEAPON_WAIT 0: the Avenger
	// accelerates every frame and its flame is continuous.
	static const ShipSpec data{
		.maxCrew = 22,
		.maxEnergy = 16,
		.energyRegen = 4,
		.energyWait = 4,
		.thrust{.max = 25, .increment = 5},
		.thrustWait = 0,
		.turnWait = 2,
		.mass = 7,
		.weapon{
			.wait = 0,
			.energyCost = 1,
			.speed = 25,  // MISSILE_SPEED == MAX_THRUST
			.life = 8,
			.damage = 1,
			.hitPoints = 1,
			.muzzleOffset = 29,  // ILWRATH_OFFSET
			.blastOffset = 0,    // MISSILE_OFFSET
			.spawn = spawnAvengerPrimary,
			.preProcess = flamePreProcess,
			.onCollision = flameCollision,
		},
		.special{
			.wait = 13,
			.energyCost = 3,
			// No post hook: the cloak is the ship's preProcess, winning the
			// energy race against the same frame's shot (see ShipSpec).
		},
		.preProcess = ilwrathPreProcess,
	};
	return data;
}

}  // namespace uqm::sim
