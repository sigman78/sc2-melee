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
	if (s.energy > s.spec->battery.max)
		s.energy = s.spec->battery.max;
	if (s.energy < 0)
		s.energy = 0;
	s.energyCounter = s.spec->battery.wait;
	return true;
}

// The silhouette follows the facing: implicit in the C (process.c:159-160,
// InitIntersectFrame reads whatever frame is displayed); explicit here,
// since the mask lives in the simulation and there's no sprite. Updates the
// Collider if one is already attached, else attaches one fresh -- a ship
// spawned with no initial mask (nullptr, e.g. a headless test) gets its
// first Collider exactly this way, on its own Appearing frame.
void
applyFacingMask(Battle &b, EntityId id, Facing facing, const ShipSpec &spec) noexcept
{
	if (spec.facingMasks.empty())
		return;
	const usize i =
			static_cast<usize>(facing.raw()) % spec.facingMasks.size();
	const CollisionMask *mask = &spec.facingMasks[i];
	if (Collider *c = b.find<Collider>(id))
		c->mask = mask;
	else
		b.attach<Collider>(id, mask);
}

EntityId
spawnPlayerShip(Battle &b, const ShipSpec &spec,
		Borrowed<const CollisionMask> mask, Vec2i at, Facing facing,
		i32 playerNr, bool warpIn)
{
	Position pos{at, at, facing};
	const Physique phys{spec.mass};
	Spawned s = b.spawn(Layer::Field, pos, Motion{}, phys, mask,
			Allegiance{playerNr, kNoEntity});
	s.with(IgnoreSimilar{});
	if (warpIn)
		s.with(WarpingIn{});
	b.attachShip(s.id(), &spec);
	return s;
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
	else if (s.energy < spec.battery.max || spec.battery.regen < 0)
	{
		(void)deltaEnergy(s, spec.battery.regen);
	}
}

// Turning. One facing step per turn_wait frames -- ships rotate in whole
// facings, which is why the trig tables matter (ship.c:238-254).
void
turnShip(Battle &b, EntityId id, ShipState &s, const ShipSpec &spec) noexcept
{
	const Input &in = *b.find<Input>(id);
	if (s.turnWait > 0)
	{
		--s.turnWait;
	}
	else if (any(in.buttons & (ShipInput::Left | ShipInput::Right)))
	{
		Position &pos = *b.find<Position>(id);
		const int delta = any(in.buttons & ShipInput::Left) ? -1 : 1;
		pos.facing += delta;
		s.turnWait = spec.turnWait;
		applyFacingMask(b, id, pos.facing, spec);
	}
}

// Thrust (ship.c:256-276). The facing is passed in rather than read off a
// global -- see Thrust.hpp.
void
applyThrustInput(Battle &b, EntityId id, ShipState &s,
		const ShipSpec &spec) noexcept
{
	const Input &in = *b.find<Input>(id);
	if (s.thrustWait > 0)
	{
		--s.thrustWait;
	}
	else if (any(in.buttons & ShipInput::Thrust))
	{
		auto [pos, motion] = b.get<Position, Motion>(id);
		const Facing facing = pos.facing;
		s.speed = thrust(motion.velocity, facing, spec.thrust,
				ThrustState{s.speed, s.inGravityWell});
		// ship.c:263-267 clears the whole speed/gravity group and ORs in the
		// thrust result; gravity re-sets the well flag next frame if still inside
		// one, so leaving the well loses the licence to exceed max speed at once.
		s.inGravityWell = false;
		s.thrustWait = spec.thrustWait;

		// Exhaust only on frames the ship actually accelerates (ship.c:274), and
		// not while FULLY cloaked (ship.c:271 gates on OBJECT_CLOAKED, true only at
		// black) -- a half-faded ship still emits one (tactrans.c:792-832).
		if (!b.has<Cloaked>(id))
			spawnIonTrail(b, id);
	}
}

// Firing. The energy is spent as part of the test, so a ship that cannot
// afford the shot does not start the cooldown either.
void
fireWeapon(Battle &b, EntityId id, ShipState &s, const ShipSpec &spec) noexcept
{
	const Input &in = *b.find<Input>(id);
	if (s.weaponCounter > 0)
	{
		--s.weaponCounter;
	}
	else if (any(in.buttons & ShipInput::Weapon)
			&& deltaEnergy(s, -spec.weapon.energyCost))
	{
		auto [shipPos, shipMotion] = b.get<Position, Motion>(id);
		ShipView view;
		view.position = shipPos.next;
		view.velocity = shipMotion.velocity;
		view.facing = shipPos.facing;
		view.playerNr = b.find<Allegiance>(id)->playerNr;
		view.weaponSpeed = spec.weapon.speed;
		view.weaponLife = spec.weapon.lifetime.remaining;
		view.weaponDamage = spec.weapon.warhead.damage;
		view.weaponHitPoints = spec.weapon.vitality.hitPoints;
		view.muzzleOffset = spec.weapon.muzzleOffset;
		view.blastOffset = spec.weapon.warhead.blastOffset;

		SpawnBuffer buf{};
		const usize n = spec.weapon.spawn != nullptr
				? spec.weapon.spawn(view, buf)
				: 0;

		for (usize i = 0; i < n; ++i)
		{
			const Spawn &sp = buf[i];
			Position wPos;
			wPos.current = wrap(sp.position);
			wPos.next = wPos.current;
			wPos.facing = sp.facing;
			Motion wMotion;
			// A weapon's mass is its damage (weapon.c:101; laser's is 1, weapon.c:58).
			// Not bookkeeping: CollisionPossible skips pairs where both masses are
			// zero, so a massless shot can't hit another shot. Warhead.damage is
			// a separate copy of the same value -- Sound.cpp's boom selection
			// reads it, not this mass.
			const Physique wPhys{sp.damage};
			// The mask follows the sprite FRAME, not the facing: same thing for the
			// nuke (16 facing cels, frameIndex = facing), different for the flame (8
			// ANIMATION cels, frameIndex 0). AnimFrame carries the frame (cmd.animFrame
			// below).
			const CollisionMask *shotMask = spec.weapon.masks.empty()
					? nullptr
					: &spec.weapon.masks[static_cast<usize>(sp.frameIndex)
							% spec.weapon.masks.size()];
			wMotion.velocity.setVector(sp.speed, sp.facing);
			// owner = id: pParent, what IGNORE_SIMILAR is tested against.
			const Allegiance wAllegiance{sp.playerNr, id};

			// Backed off by one frame of its own muzzle velocity, as the C does for
			// every missile (weapon.c:126-127); the catch-up pass integrates it
			// forward, so total travel over its life is life frames, not life + 1.
			{
				const Vec2i v0 = wMotion.velocity.current();
				wPos.current = wrap(Vec2i{wPos.current.x - velocityToWorld(v0.x),
						wPos.current.y - velocityToWorld(v0.y)});
				wPos.next = wPos.current;
			}

			if (sp.inheritsVelocity)
			{
				// The ship's velocity adds on top of the muzzle velocity, and the start
				// position backs off by one frame of it (ilwrath.c:219-222) -- otherwise
				// the first frame of flame appears ahead of where the ship actually was.
				const Vec2i v = shipMotion.velocity.current();
				wMotion.velocity.deltaComponents(v.x, v.y);
				wPos.current = wrap(Vec2i{wPos.current.x - velocityToWorld(v.x),
						wPos.current.y - velocityToWorld(v.y)});
				wPos.next = wPos.current;
			}

			// Queued, not spawned: it enters the world at the sync point and
			// takes its first live frame the step after this one, one frame
			// later than the C's same-step catch-up gave it (review-006
			// §4's accepted latency).
			b.queueSpawn(SpawnCommand{
					.layer = Layer::Ordnance,
					.position = wPos,
					.motion = wMotion,
					.physique = wPhys,
					.allegiance = wAllegiance,
					.weaponSpec = &spec.weapon,

					// Attached verbatim (review-007 W9): the spec's own literal
					// already carries the wound clock (initialize_nuke seeds
					// TRACK_WAIT, human.c:297-299), so there is nothing left to
					// reassemble at fire time -- presence is std::optional's own
					// question now, not an "any of three scalars nonzero" test.
					.guided = spec.weapon.guided,

					.ignoreSimilar = sp.ignoreSimilar,
					.lifetime = Lifetime{sp.life},
					.vitality = Vitality{sp.hitPoints},
					.warhead = Warhead{sp.damage, sp.blastOffset,
							spec.weapon.warhead.lingersOnHit},
					.animFrame = AnimFrame{sp.frameIndex},
					.frameDriven = spec.weapon.frameDriven,
					.collider = shotMask,
			});
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
	// WarpingIn/Appearing are presence filters on the iterated ship, so they
	// belong in the query (SiGMan's review), not an in-body has<>/!has<>
	// guard; crew == 0 is a value test and stays in the body.
	b.view<ShipState>(entt::exclude<WarpingIn, Appearing>).each(
			[](ShipState &s) {
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
	if (!b.alive(id))
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

	if (b.has<Appearing>(id))
	{
		Input &in = *b.find<Input>(id);
		s.crew = spec.maxCrew;
		s.energy = spec.battery.max;
		in.buttons = ShipInput::None;
		auto [allegiance, pos] = b.get<Allegiance, Position>(id);
		allegiance.owner = id;
		applyFacingMask(b, id, pos.facing, spec);
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

}  // namespace

void
shipMachinesPass(Battle &b) noexcept
{
	b.eachOrdered([&b](EntityId id) {
		if (b.has<ShipState>(id))
			shipMachinesStep(b, id);
	});
}

void
turnPass(Battle &b) noexcept
{
	// ShipState alone in the join already selects exactly the ships --
	// only a ship ever carries one. WarpingIn/Appearing are presence
	// filters -> the query's exclude; crew == 0 is a value test -> stays in
	// the body (review-007 W4b's join rule, and SiGMan's
	// presence-in-the-query review). Order-free: turning has no
	// cross-entity or spawn-ordering dependency. Element itself dropped from
	// the join now that turnWait moved to ShipState -- turnShip reads
	// nothing else off it.
	b.view<ShipState>(entt::exclude<WarpingIn, Appearing>).each(
			[&b](EntityId id, ShipState &s) {
				if (s.crew == 0)
					return;
				turnShip(b, id, s, *s.spec);
			});
}

// A thrusting ship queues an ion-trail spawn command (spawnIonTrail), and
// that command's position in spawnCommands_ fixes the trail's Order (layer,
// seq) slot at the sync point -- eachOrdered's emission order matches, so
// this is clean again (the ShipState view's storage order was what broke
// --compare when this pass first went idiomatic).
void
thrustPass(Battle &b) noexcept
{
	b.eachOrdered<ShipState>(entt::exclude<WarpingIn, Appearing>,
			[&b](EntityId id, ShipState &s) {
				if (s.crew == 0)
					return;
				applyThrustInput(b, id, s, *s.spec);
			});
}

void
fireAndSpecialGatePass(Battle &b) noexcept
{
	// No Appearing exclusion here, unlike Turn/Thrust: ShipMachines already
	// forces Input::None on the appearing frame, so fireWeapon/gateSpecial
	// see nothing pressed regardless. Neither reads Element any more (the
	// ship's own playerNr moved to Allegiance, fetched inside fireWeapon),
	// so the join shrinks to just ShipState.
	b.eachOrdered<ShipState>(entt::exclude<WarpingIn>,
			[&b](EntityId id, ShipState &s) {
				if (s.crew == 0)
					return;
				fireWeapon(b, id, s, *s.spec);
				gateSpecial(b, id, s, *s.spec);
			});
}

void
guidedSteerPass(Battle &b) noexcept
{
	b.eachOrdered([&b](EntityId id) {
		if (b.has<Guided>(id))
			guidedShotPreProcess(b, id);
	});
}

void
guidedShotPreProcess(Battle &b, EntityId id) noexcept
{
	if (!b.alive(id))
		return;
	Position *pos = b.find<Position>(id);
	Borrowed<const WeaponSpec> ws = b.weaponSpec(id);
	Guided *g = b.find<Guided>(id);
	if (ws == nullptr || g == nullptr)
		return;

	// Steer, but only every TRACK_WAIT frames (human.c:133-146). The clock
	// is the component's own, not a repurposed Element::turnWait.
	Facing facing = pos->facing;
	if (g->clock > 0)
	{
		--g->clock;
	}
	else
	{
		(void)trackShip(b, id, facing);
		if (!b.alive(id))
			return;
		pos = b.find<Position>(id);
		pos->facing = facing;
		g->clock = g->trackWait;

		// The mask follows the facing too, or a steering nuke's rect keeps the
		// launch cel's size while the sprite changes cel. AnimFrame is the cel
		// the renderer draws.
		b.find<AnimFrame>(id)->n = pos->facing.raw();
		if (!ws->masks.empty())
		{
			if (Collider *c = b.find<Collider>(id))
				c->mask = &ws->masks[static_cast<usize>(pos->facing.raw())
						% ws->masks.size()];
		}
	}

	// Accelerates as it goes (human.c:148-157): speed climbs with life spent,
	// capped -- a nuke chasing you a while is much harder to outrun than one
	// just launched.
	i32 speed = ws->speed
			+ (ws->lifetime.remaining - lifeSpanOf(b, id)) * g->thrustScale;
	if (speed > g->maxSpeed)
		speed = g->maxSpeed;
	b.find<Motion>(id)->velocity.setVector(speed, pos->facing);
}

void
spawnIonTrail(Battle &b, EntityId ship) noexcept
{
	if (!b.alive(ship))
		return;
	const Position &shipPos = *b.find<Position>(ship);

	// Behind the ship, along the reverse of its facing. The C offsets by the
	// sprite's height so the exhaust leaves the hull rather than the hotspot
	// (tactrans.c:808-812); the collision mask stands in for the frame rect.
	const Angle angle = shipPos.facing.angle().opposite();
	const Collider *hull = b.find<Collider>(ship);
	const i32 back = hull != nullptr
			? displayToWorld(static_cast<i32>(hull->mask->size().h) / 2)
			: 0;

	// NEUTRAL: exhaust belongs to nobody -- cmd.allegiance below defaults
	// to exactly that (playerNr -1, no owner), so nothing sets it here. No
	// AnimFrame either: an ion trail animates by Lifetime::remaining
	// (Draw.cpp's RampPoint), and nothing ever read its old colorCycle.
	Position pos;
	pos.current = wrap(Vec2i{shipPos.current.x + cosine(angle, back),
			shipPos.current.y + sine(angle, back)});
	pos.next = pos.current;

	// Queued, not spawned: Background layer so it draws behind everything
	// that matters, once it exists next frame (review-006 §4).
	b.queueSpawn(SpawnCommand{
			.layer = Layer::Background,
			.position = pos,
			.effect = true,  // stationary: no Motion needed (review-007 W5)
			.trail = true,
			.lifetime = Lifetime{Trail::kLife},
	});
}

namespace {

void
warpInStep(Battle &b, EntityId id) noexcept
{
	if (!b.alive(id))
		return;
	ShipState *sp = b.ship(id);
	if (sp == nullptr)
		return;

	if (b.has<Appearing>(id))
	{
		// Arriving: invisible, untouchable, on a clock (tactrans.c:858-866). The
		// ship is in the simulation the whole time -- just not hittable or drawn --
		// which stops two ships materialising inside each other. The Collider
		// stays attached throughout; collidable() excludes it by WarpingIn
		// instead, so there is no mask to lose track of and nothing to restore
		// on arrival.
		sp->crew = sp->spec->maxCrew;
		sp->energy = sp->spec->battery.max;
		auto [in, allegiance] = b.get<Input, Allegiance>(id);
		in.buttons = ShipInput::None;
		allegiance.owner = id;
		b.attach<Lifetime>(id, Lifetime{WarpingIn::kFrames});
		b.find<Motion>(id)->velocity.zero();
		return;
	}

	// The trail *is* the ship teleporting in: each frame drops a stationary
	// hull copy behind the arrival point, shrinking by TRANSITION_SPEED per
	// frame left (tactrans.c:938-950), so images march inward to the ship.
	// It never carries a Collider -- it was never collidable, and app/melee's
	// Draw.cpp draws it hull-sized straight from content, not from here.
	{
		const Position &shipPos = *b.find<Position>(id);
		const Angle angle = shipPos.facing.angle();
		const i32 back = WarpingIn::kImageSpacing * (lifeSpanOf(b, id) - 1);

		Position shadowPos;
		shadowPos.facing = shipPos.facing;
		shadowPos.current = wrap(Vec2i{shipPos.current.x - cosine(angle, back),
				shipPos.current.y - sine(angle, back)});
		shadowPos.next = shadowPos.current;

		b.queueSpawn(SpawnCommand{
				.layer = Layer::Background,
				.position = shadowPos,
				// Picks which ship's sprites to draw; no owner of its own.
				.allegiance = Allegiance{b.find<Allegiance>(id)->playerNr, kNoEntity},
				.effect = true,  // stationary: no Motion needed (review-007 W5)
				.shadow = true,
				.lifetime = Lifetime{Trail::kLife},
		});
	}

	if (!b.alive(id))
		return;

	if (lifeSpanOf(b, id) <= 1)
	{
		// Arrived: solid, visible, under its own control (tactrans.c:868-886).
		// The Collider was never removed, so applyFacingMask's rebuild from
		// the spec's facingMasks is a refresh, not a reattach; a spec with
		// none to rebuild from (a headless test or replay ship, given its
		// mask directly at spawn) simply leaves the Collider's mask as it
		// already was.
		b.detach<Lifetime>(id);  // NORMAL_LIFE: persistent again
		auto [motion, pos] = b.get<Motion, Position>(id);
		motion.velocity.zero();
		applyFacingMask(b, id, pos.facing, *sp->spec);
		b.detach<WarpingIn>(id);
	}
}

}  // namespace

// cleanup_dead_ship (tactrans.c:307-337): when the wreck finishes burning,
// everything the dead ship still owns -- in-flight nukes, the flame stream --
// goes with it. The C excepts drifting crew, which does not exist yet.
// External linkage (review-007 W5): the death path (Battle.cpp) calls this
// directly for any SweepsOwnedOnDeath entity, replacing the old onDeath
// function-pointer hook.
void
sweepDeadShipOrdnance(Battle &b, EntityId id) noexcept
{
	// Allegiance is a required join now, not a get-then-null-check: every
	// live entity has one (the uniform attach), so requiring it in
	// eachOrdered<Allegiance> is the same filter, just declared instead of
	// checked.
	b.eachOrdered<Allegiance>([&b, id](EntityId other, Allegiance &a) {
		if (other == id || !(a.owner == id))
			return;
		b.attachOrReplace<Lifetime>(other, Lifetime{0});
		b.attachOrReplace<Doomed>(other);
		b.detach<Collider>(other);
	});
}

void
startShipExplosion(Battle &b, EntityId id) noexcept
{
	if (!b.alive(id))
		return;

	// The ship becomes its own explosion rather than vanishing
	// (tactrans.c:703-728): stops dead, loses energy, stops colliding, and
	// burns for a fixed number of frames.
	b.find<Motion>(id)->velocity.zero();
	if (ShipState *sp = b.ship(id))
		sp->energy = 0;
	// A dying ship draws ByFacing (Draw.cpp), not ByFrame, so it never had
	// an AnimFrame to reset -- the old colorCycle = 0 here was dead too.
	b.detach<Doomed>(id);
	b.attach<Lifetime>(id, Lifetime{Exploding::kLife});
	b.detach<Collider>(id);
	if (!b.has<SweepsOwnedOnDeath>(id))
		b.attach<SweepsOwnedOnDeath>(id);
	if (!b.has<Exploding>(id))
		b.attach<Exploding>(id);
}

namespace {

void
explosionStep(Battle &b, EntityId id) noexcept
{
	if (!b.alive(id))
		return;

	// How many sparks this frame: the C's schedule (tactrans.c:545-575) ramps
	// 1/3/1 over the 26 frames it spawns for, then nothing for the last ten
	// while thrown sparks finish burning.
	const i32 age = Exploding::kLife - lifeSpanOf(b, id);
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

	const Vec2i from = b.find<Position>(id)->current;
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

		// NEUTRAL: wreckage belongs to nobody -- cmd.allegiance below
		// defaults to exactly that.
		Position dPos;
		dPos.current = wrap(Vec2i{from.x + cosine(spot, dist),
				from.y + sine(spot, dist)});
		dPos.next = dPos.current;
		// From components at the full 64-angle resolution, the way the C's
		// SetVelocityComponents call is (tactrans.c:607-612). Rounding the
		// drift to 16 facings visibly banded the cloud.
		Motion dMotion;
		dMotion.velocity.setComponents(cosine(drift, worldToVelocity(speed)),
				sine(drift, worldToVelocity(speed)));

		b.queueSpawn(SpawnCommand{
				.layer = Layer::Background,
				.position = dPos,
				.motion = dMotion,
				// Never collidable, but the one decoration that drifts, so it needs
				// Motion where the others don't (review-007 W5's diet).
				.effect = true,
				.effectMoves = true,
				.debris = true,
				.lifetime = Lifetime{Debris::kLife},
		});
	}
}

}  // namespace

}  // namespace uqm::sim
