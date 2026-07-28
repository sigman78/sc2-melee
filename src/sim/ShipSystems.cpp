// Copyright the Ur-Quan Masters contributors. GPL-2.0-or-later.

#include "ShipSystems.hpp"

#include "engine/core/Types.hpp"
#include "sim/Battle.hpp"
#include "sim/Damage.hpp"
#include "sim/Targeting.hpp"
#include "sim/Trig.hpp"

#include <utility>

namespace uqm::sim {

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

namespace {

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

			// Queued, not spawned: it enters the world at the sync point and
			// takes its first live frame the step after this one, one frame
			// later than the C's same-step catch-up gave it (review-006
			// §4's accepted latency).
			SpawnCommand cmd;
			cmd.layer = Layer::Ordnance;
			cmd.weaponSpec = &spec.weapon;

			// A guided shot starts with its tracking clock already wound:
			// initialize_nuke seeds TRACK_WAIT (human.c:297-299), so the
			// first steer lands on the fourth hook frame. The spec declares
			// guidance by having any of the numbers; the component carries
			// them plus the shot's own clock.
			if (spec.weapon.trackWait > 0 || spec.weapon.thrustScale > 0)
			{
				cmd.guided = Guided{spec.weapon.trackWait, spec.weapon.maxSpeed,
						spec.weapon.thrustScale, spec.weapon.trackWait};
			}
			cmd.element = std::move(w);
			b.queueSpawn(std::move(cmd));
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
energyRegenPass(Battle &b) noexcept
{
	b.eachShip([&b](EntityId id, ShipState &s) {
		if (b.has<WarpingIn>(id))
			return;
		const Element *e = b.get(id);
		if (e == nullptr || any(e->flags & ElementFlags::Appearing))
			return;
		if (s.crew == 0)
			return;
		regenEnergy(s, *s.spec);
	});
}

namespace {

// ShipMachines (pipeline slot 4), one ship: warping in pre-empts
// everything, the appearing frame is its own one-time init, a dead hull
// only burns, and only what is left of those runs the ship's own preProcess
// hook (the Ilwrath cloak). Turn and Thrust are their own passes below.
void
shipMachinesStep(Battle &b, EntityId id) noexcept
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
	{
		warpInStep(b, id);
		return;
	}

	if (any(e->flags & ElementFlags::Appearing))
	{
		Input &in = *b.find<Input>(id);
		s.crew = spec.maxCrew;
		s.energy = spec.maxEnergy;
		in.buttons = ShipInput::None;
		e->owner = id;
		applyFacingMask(*e, spec);
		return;
	}

	if (s.crew == 0)
	{
		if (b.has<Exploding>(id))
			explosionStep(b, id);
		return;
	}

	if (spec.preProcess != nullptr)
		spec.preProcess(b, id);
}

// The skip list every ship pass below applies: warping in, appearing, or
// dead hulls run none of Turn/Thrust/Fire/SpecialGate.
[[nodiscard]] bool
shouldSkipShipFrame(Battle &b, EntityId id, const Element &e,
		const ShipState &s, bool checkAppearing) noexcept
{
	if (b.has<WarpingIn>(id))
		return true;
	if (checkAppearing && any(e.flags & ElementFlags::Appearing))
		return true;
	return s.crew == 0;
}

}  // namespace

void
shipMachinesPass(Battle &b) noexcept
{
	for (EntityId id = b.front(); id != kNoEntity; id = b.next(id))
		if (b.has<PlayerShip>(id))
			shipMachinesStep(b, id);
}

void
turnPass(Battle &b) noexcept
{
	b.eachElementWith<ShipState>([&b](EntityId id, Element &e, ShipState &s) {
		if (shouldSkipShipFrame(b, id, e, s, true))
			return;
		turnShip(b, id, e, s, *s.spec);
	});
}

// Stays a spine walk, unlike Turn: a thrusting ship queues an ion-trail
// spawn command (spawnIonTrail), and that command's position in
// spawnCommands_ fixes the trail's Seq/layer-FIFO slot at the sync point --
// the ShipState view's storage order does not match the spine's, and
// switching this one broke replay_test's --compare within the first
// checkpoint window on 30 of 32 battles.
void
thrustPass(Battle &b) noexcept
{
	for (EntityId id = b.front(); id != kNoEntity; id = b.next(id))
	{
		if (!b.has<PlayerShip>(id))
			continue;
		auto e = b.get(id);
		ShipState *sp = b.ship(id);
		if (e == nullptr || sp == nullptr
				|| shouldSkipShipFrame(b, id, *e, *sp, true))
			continue;
		applyThrustInput(b, id, *e, *sp, *sp->spec);
	}
}

void
fireAndSpecialGatePass(Battle &b) noexcept
{
	for (EntityId id = b.front(); id != kNoEntity; id = b.next(id))
	{
		if (!b.has<PlayerShip>(id))
			continue;
		auto e = b.get(id);
		ShipState *sp = b.ship(id);
		// No Appearing check here, unlike Turn/Thrust: ShipMachines already
		// forces Input::None on the appearing frame, so fireWeapon/
		// gateSpecial see nothing pressed regardless.
		if (e == nullptr || sp == nullptr
				|| shouldSkipShipFrame(b, id, *e, *sp, false))
			continue;
		fireWeapon(b, id, *e, *sp, *sp->spec);
		gateSpecial(b, id, *sp, *sp->spec);
	}
}

void
guidedSteerPass(Battle &b) noexcept
{
	for (EntityId id = b.front(); id != kNoEntity; id = b.next(id))
		if (b.has<Guided>(id))
			guidedShotPreProcess(b, id);
}

void
guidedShotPreProcess(Battle &b, EntityId id) noexcept
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

	// Queued, not spawned: Background layer so it draws behind everything
	// that matters, once it exists next frame (review-006 §4).
	SpawnCommand cmd;
	cmd.layer = Layer::Background;
	cmd.element = std::move(t);
	cmd.ignoreVelocity = true;
	b.queueSpawn(std::move(cmd));
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

		SpawnCommand cmd;
		cmd.layer = Layer::Background;
		cmd.element = std::move(shadow);
		b.queueSpawn(std::move(cmd));
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

		SpawnCommand cmd;
		cmd.layer = Layer::Background;
		cmd.element = std::move(d);
		b.queueSpawn(std::move(cmd));
	}
}

}  // namespace

bool
isCloaked(const Battle &b, EntityId id) noexcept
{
	// OBJECT_CLOAKED is STAMPFILL *and* BLACK (element.h:201-204): hidden
	// only when fully faded, in either direction of the walk.
	const Cloak *c = b.find<const Cloak>(id);
	return c != nullptr && c->level == kCloakFullLevel;
}

}  // namespace uqm::sim
