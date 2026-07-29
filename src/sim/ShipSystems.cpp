// Copyright the Ur-Quan Masters contributors. GPL-2.0-or-later.

#include "ShipSystems.hpp"

#include "engine/core/Types.hpp"
#include "sim/Battle.hpp"
#include "sim/Damage.hpp"
#include "sim/Targeting.hpp"
#include "sim/Trig.hpp"

#include <algorithm>
#include <utility>

namespace uqm::sim {

// Spends or restores energy, reporting whether it could; firing gates on
// success (ship.c:296-299). Every success re-arms the regen countdown
// (status.c:317-323); a failed spend does not.
bool deltaEnergy(comp::ShipState &s, i32 delta) noexcept
{
	if (delta < 0 && s.energy + delta < 0)
		return false;

	s.energy += delta;
	s.energy = std::min(s.energy, s.spec->battery.max);
	s.energy = std::max(s.energy, 0);
	s.energyCounter = s.spec->battery.wait;
	return true;
}

// The silhouette follows the facing (process.c:159-160). Updates the
// Collider if one is already attached, else attaches one fresh -- a ship
// spawned with no initial mask gets its first Collider this way.
void applyFacingMask(
		Battle &b, EntityId id, Facing facing, const ShipSpec &spec) noexcept
{
	if (spec.facingMasks.empty())
		return;
	const usize i = static_cast<usize>(facing.raw()) % spec.facingMasks.size();
	const CollisionMask *mask = &spec.facingMasks[i];
	if (comp::Collider *c = b.reg.try_get<comp::Collider>(id))
		c->mask = mask;
	else
		b.reg.emplace<comp::Collider>(id, mask);
}

EntityId spawnPlayerShip(Battle &b, const ShipSpec &spec,
		Borrowed<const CollisionMask> mask, Vec2i at, Facing facing,
		i32 playerNr, bool warpIn)
{
	comp::Position const pos{at, at, facing};
	const comp::Physique phys{spec.mass};
	Spawned s = b.spawn(Layer::Field, pos, comp::Motion{}, phys, mask,
			comp::Allegiance{playerNr, kNoEntity});
	s.with(comp::IgnoreSimilar{});
	if (warpIn)
		s.with(comp::WarpingIn{});
	b.attachShip(s.id(), &spec);
	return s;
}

namespace {

// Energy regeneration, gated by its own counter (ship.c:225-230). The
// counter itself is re-armed inside deltaEnergy on every success, which is
// how firing postpones regeneration.
void regenEnergy(comp::ShipState &s, const ShipSpec &spec) noexcept
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
void turnShip(Battle &b, EntityId id, comp::ShipState &s,
		const ShipSpec &spec) noexcept
{
	const comp::Input &in = b.reg.get<comp::Input>(id);
	if (s.turnWait > 0)
	{
		--s.turnWait;
	}
	else if (any(in.buttons & (ShipInput::Left | ShipInput::Right)))
	{
		comp::Position &pos = b.reg.get<comp::Position>(id);
		const int delta = any(in.buttons & ShipInput::Left) ? -1 : 1;
		pos.facing += delta;
		s.turnWait = spec.turnWait;
		applyFacingMask(b, id, pos.facing, spec);
	}
}

// Thrust (ship.c:256-276). The facing is passed in rather than read off a
// global -- see Thrust.hpp.
void applyThrustInput(Battle &b, EntityId id, comp::ShipState &s,
		const ShipSpec &spec) noexcept
{
	const comp::Input &in = b.reg.get<comp::Input>(id);
	if (s.thrustWait > 0)
	{
		--s.thrustWait;
	}
	else if (any(in.buttons & ShipInput::Thrust))
	{
		auto [pos, motion] = b.reg.get<comp::Position, comp::Motion>(id);
		const Facing facing = pos.facing;
		s.speed = thrust(motion.velocity, facing, spec.thrust,
				ThrustState{s.speed, s.inGravityWell});
		// ship.c:263-267 clears the whole speed/gravity group and ORs in the
		// thrust result; gravity re-sets the well flag next frame if still
		// inside one, so leaving the well loses the licence to exceed max speed
		// at once.
		s.inGravityWell = false;
		s.thrustWait = spec.thrustWait;

		// Exhaust only on frames the ship actually accelerates (ship.c:274),
		// and not while FULLY cloaked (ship.c:271 gates on OBJECT_CLOAKED, true
		// only at black) -- a half-faded ship still emits one
		// (tactrans.c:792-832).
		if (!b.reg.all_of<comp::Cloaked>(id))
			spawnIonTrail(b, id);
	}
}

// Firing. The energy is spent as part of the test, so a ship that cannot
// afford the shot does not start the cooldown either.
void fireWeapon(Battle &b, EntityId id, comp::ShipState &s,
		const ShipSpec &spec) noexcept
{
	const comp::Input &in = b.reg.get<comp::Input>(id);
	if (s.weaponCounter > 0)
	{
		--s.weaponCounter;
	}
	else if (any(in.buttons & ShipInput::Weapon)
			&& deltaEnergy(s, -spec.weapon.energyCost))
	{
		auto [shipPos, shipMotion] =
				b.reg.get<comp::Position, comp::Motion>(id);
		ShipView view;
		view.position = shipPos.next;
		view.velocity = shipMotion.velocity;
		view.facing = shipPos.facing;
		view.playerNr = b.reg.get<comp::Allegiance>(id).playerNr;
		view.weaponSpeed = spec.weapon.speed;
		view.weaponLife = spec.weapon.lifetime.remaining;
		view.weaponDamage = spec.weapon.warhead.damage;
		view.weaponHitPoints = spec.weapon.vitality.hitPoints;
		view.muzzleOffset = spec.weapon.muzzleOffset;
		view.blastOffset = spec.weapon.warhead.blastOffset;

		SpawnBuffer buf{};
		const usize n =
				spec.weapon.spawn != nullptr ? spec.weapon.spawn(view, buf) : 0;

		for (usize i = 0; i < n; ++i)
		{
			const Spawn &sp = buf[i];
			comp::Position wPos;
			wPos.current = wrap(sp.position);
			wPos.next = wPos.current;
			wPos.facing = sp.facing;
			comp::Motion wMotion;
			// A weapon's mass is its damage (weapon.c:101) -- CollisionPossible
			// skips pairs where both masses are zero, so a massless shot can't
			// hit another. Warhead.damage is a separate copy Sound.cpp reads.
			const comp::Physique wPhys{sp.damage};
			// The mask follows the sprite FRAME, not the facing: same value for
			// the nuke (16 facing cels, frameIndex = facing), different for the
			// flame (8 animation cels, frameIndex 0).
			const CollisionMask *shotMask = spec.weapon.masks.empty()
					? nullptr
					: &spec.weapon.masks[static_cast<usize>(sp.frameIndex)
							  % spec.weapon.masks.size()];
			wMotion.velocity.setVector(sp.speed, sp.facing);
			// owner = id: pParent, what IGNORE_SIMILAR is tested against.
			const comp::Allegiance wAllegiance{sp.playerNr, id};

			// Backed off by one frame of its own muzzle velocity, as the C does
			// for every missile (weapon.c:126-127); the catch-up pass
			// integrates it forward, so total travel over its life is life
			// frames, not life + 1.
			{
				const Vec2i v0 = wMotion.velocity.current();
				wPos.current =
						wrap(Vec2i{wPos.current.x - velocityToWorld(v0.x),
								wPos.current.y - velocityToWorld(v0.y)});
				wPos.next = wPos.current;
			}

			if (sp.inheritsVelocity)
			{
				// The ship's velocity adds on top of the muzzle velocity, and
				// the start position backs off by one frame of it
				// (ilwrath.c:219-222) -- otherwise the first frame of flame
				// appears ahead of where the ship actually was.
				const Vec2i v = shipMotion.velocity.current();
				wMotion.velocity.deltaComponents(v.x, v.y);
				wPos.current = wrap(Vec2i{wPos.current.x - velocityToWorld(v.x),
						wPos.current.y - velocityToWorld(v.y)});
				wPos.next = wPos.current;
			}

			// Queued, not spawned: it enters the world at the sync point and
			// takes its first live frame the step after this one, one frame
			// later than the C's same-step catch-up gave it.
			b.queueSpawn(SpawnCommand{
					.layer = Layer::Ordnance,
					.position = wPos,
					.motion = wMotion,
					.physique = wPhys,
					.allegiance = wAllegiance,
					.weaponSpec = &spec.weapon,

					// Attached verbatim: the spec's own literal already carries
					// the wound clock (initialize_nuke seeds TRACK_WAIT,
					// human.c:297-299).
					.guided = spec.weapon.guided,

					.ignoreSimilar = sp.ignoreSimilar,
					.lifetime = comp::Lifetime{sp.life},
					.vitality = comp::Vitality{sp.hitPoints},
					.warhead = comp::Warhead{sp.damage, sp.blastOffset,
							spec.weapon.warhead.lingersOnHit},
					.animFrame = comp::AnimFrame{sp.frameIndex},
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
void gateSpecial(Battle &b, EntityId id, comp::ShipState &s,
		const ShipSpec &spec) noexcept
{
	const comp::Input &in = b.reg.get<comp::Input>(id);
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

void energyRegenPass(Battle &b) noexcept
{
	// WarpingIn/Appearing are presence filters, so they belong in the query
	// exclusion; crew == 0 is a value test and stays in the body.
	b.reg.view<comp::ShipState>(entt::exclude<comp::WarpingIn, comp::Appearing>)
			.each([](comp::ShipState &s) {
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
void shipMachinesStep(Battle &b, EntityId id, comp::ShipState &s) noexcept
{
	const ShipSpec &spec = *s.spec;

	if (b.reg.all_of<comp::WarpingIn>(id))
	{
		warpInStep(b, id);
		return;
	}

	if (b.reg.all_of<comp::Appearing>(id))
	{
		comp::Input &in = b.reg.get<comp::Input>(id);
		s.crew = spec.maxCrew;
		s.energy = spec.battery.max;
		in.buttons = ShipInput::None;
		auto [allegiance, pos] =
				b.reg.get<comp::Allegiance, comp::Position>(id);
		allegiance.owner = id;
		applyFacingMask(b, id, pos.facing, spec);
		return;
	}

	if (s.crew == 0)
	{
		if (b.reg.all_of<comp::Exploding>(id))
			explosionStep(b, id);
		return;
	}

	if (spec.preProcess != nullptr)
		spec.preProcess(b, id);
}

}  // namespace

void shipMachinesPass(Battle &b) noexcept
{
	b.eachOrdered<comp::ShipState>([&b](EntityId id, comp::ShipState &s) {
		shipMachinesStep(b, id, s);
	});
}

void turnPass(Battle &b) noexcept
{
	// ShipState alone selects the ships -- only a ship ever carries one.
	// WarpingIn/Appearing are presence filters in the query's exclude;
	// crew == 0 is a value test and stays in the body. Order-free: turning
	// has no cross-entity or spawn-ordering dependency.
	b.reg.view<comp::ShipState>(entt::exclude<comp::WarpingIn, comp::Appearing>)
			.each([&b](EntityId id, comp::ShipState &s) {
				if (s.crew == 0)
					return;
				turnShip(b, id, s, *s.spec);
			});
}

// A thrusting ship queues an ion-trail spawn command; that command's
// position in spawnCommands_ fixes the trail's Order (layer, seq) slot at
// the sync point, matching eachOrdered's emission order.
void thrustPass(Battle &b) noexcept
{
	b.eachOrdered<comp::ShipState>(
			entt::exclude<comp::WarpingIn, comp::Appearing>,
			[&b](EntityId id, comp::ShipState &s) {
				if (s.crew == 0)
					return;
				applyThrustInput(b, id, s, *s.spec);
			});
}

void fireAndSpecialGatePass(Battle &b) noexcept
{
	// No Appearing exclusion here, unlike Turn/Thrust: ShipMachines already
	// forces Input::None on the appearing frame, so fireWeapon/gateSpecial
	// see nothing pressed regardless.
	b.eachOrdered<comp::ShipState>(entt::exclude<comp::WarpingIn>,
			[&b](EntityId id, comp::ShipState &s) {
				if (s.crew == 0)
					return;
				fireWeapon(b, id, s, *s.spec);
				gateSpecial(b, id, s, *s.spec);
			});
}

void guidedSteerPass(Battle &b) noexcept
{
	b.eachOrdered<comp::Guided>(
			[&b](EntityId id, comp::Guided &) { guidedShotPreProcess(b, id); });
}

void guidedShotPreProcess(Battle &b, EntityId id) noexcept
{
	if (!b.alive(id))
		return;
	comp::Position *pos = &b.reg.get<comp::Position>(id);
	Borrowed<const WeaponSpec> ws = b.weaponSpec(id);
	comp::Guided *g = b.reg.try_get<comp::Guided>(id);
	if (ws == nullptr || g == nullptr)
		return;

	// Steer, but only every TRACK_WAIT frames (human.c:133-146).
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
		pos = &b.reg.get<comp::Position>(id);
		pos->facing = facing;
		g->clock = g->trackWait;

		// The mask follows the facing too, or a steering nuke's rect keeps the
		// launch cel's size while the sprite changes cel. AnimFrame is the cel
		// the renderer draws.
		b.reg.get<comp::AnimFrame>(id).n = pos->facing.raw();
		if (!ws->masks.empty())
		{
			if (comp::Collider *c = b.reg.try_get<comp::Collider>(id))
				c->mask = &ws->masks[static_cast<usize>(pos->facing.raw())
						% ws->masks.size()];
		}
	}

	// Accelerates as it goes (human.c:148-157): speed climbs with life spent,
	// capped -- a nuke chasing you a while is much harder to outrun than one
	// just launched.
	i32 speed =
			ws->speed + ageOf(b, id, ws->lifetime.remaining) * g->thrustScale;
	speed = std::min(speed, g->maxSpeed);
	b.reg.get<comp::Motion>(id).velocity.setVector(speed, pos->facing);
}

void spawnIonTrail(Battle &b, EntityId ship) noexcept
{
	if (!b.alive(ship))
		return;
	const comp::Position &shipPos = b.reg.get<comp::Position>(ship);

	// Behind the ship, along the reverse of its facing. The C offsets by the
	// sprite's height so the exhaust leaves the hull rather than the hotspot
	// (tactrans.c:808-812); the collision mask stands in for the frame rect.
	const Angle angle = shipPos.facing.angle().opposite();
	const comp::Collider *hull = b.reg.try_get<comp::Collider>(ship);
	const i32 back = hull != nullptr
			? displayToWorld(static_cast<i32>(hull->mask->size().h) / 2)
			: 0;

	// NEUTRAL: exhaust belongs to nobody, so nothing sets allegiance here.
	// No AnimFrame either: an ion trail animates by Lifetime::remaining
	// (Draw.cpp's RampPoint).
	comp::Position pos;
	pos.current = wrap(Vec2i{shipPos.current.x + cosine(angle, back),
			shipPos.current.y + sine(angle, back)});
	pos.next = pos.current;

	// Queued, not spawned: Background layer so it draws behind everything
	// that matters, once it exists next frame.
	b.queueSpawn(SpawnCommand{
			.layer = Layer::Background,
			.position = pos,
			.effect = true,  // stationary: no Motion needed
			.trail = true,
			.lifetime = comp::Lifetime{comp::Trail::kLife},
	});
}

namespace {

void warpInStep(Battle &b, EntityId id) noexcept
{
	if (!b.alive(id))
		return;
	comp::ShipState *sp = b.ship(id);
	if (sp == nullptr)
		return;

	if (b.reg.all_of<comp::Appearing>(id))
	{
		// Arriving: invisible, untouchable, on a clock (tactrans.c:858-866).
		// The Collider stays attached throughout -- collidable() excludes it
		// by WarpingIn instead, so there's nothing to restore on arrival.
		sp->crew = sp->spec->maxCrew;
		sp->energy = sp->spec->battery.max;
		auto [in, allegiance] = b.reg.get<comp::Input, comp::Allegiance>(id);
		in.buttons = ShipInput::None;
		allegiance.owner = id;
		b.reg.emplace<comp::Lifetime>(
				id, comp::Lifetime{comp::WarpingIn::kFrames});
		b.reg.get<comp::Motion>(id).velocity.zero();
		return;
	}

	// The trail *is* the ship teleporting in: each frame drops a stationary
	// hull copy behind the arrival point, shrinking by TRANSITION_SPEED per
	// frame left (tactrans.c:938-950); it never carries a Collider.
	{
		const comp::Position &shipPos = b.reg.get<comp::Position>(id);
		const Angle angle = shipPos.facing.angle();
		const i32 back =
				comp::WarpingIn::kImageSpacing * (framesLeft(b, id) - 1);

		comp::Position shadowPos;
		shadowPos.facing = shipPos.facing;
		shadowPos.current = wrap(Vec2i{shipPos.current.x - cosine(angle, back),
				shipPos.current.y - sine(angle, back)});
		shadowPos.next = shadowPos.current;

		b.queueSpawn(SpawnCommand{
				.layer = Layer::Background,
				.position = shadowPos,
				// Picks which ship's sprites to draw; no owner of its own.
				.allegiance =
						comp::Allegiance{
								b.reg.get<comp::Allegiance>(id).playerNr,
								kNoEntity},
				.effect = true,  // stationary: no Motion needed
				.shadow = true,
				.lifetime = comp::Lifetime{comp::Trail::kLife},
		});
	}

	if (!b.alive(id))
		return;

	if (framesLeft(b, id) <= 1)
	{
		// Arrived: solid, visible, under its own control (tactrans.c:868-886).
		// The Collider was never removed, so applyFacingMask's rebuild is a
		// refresh, not a reattach; a spec with no facingMasks leaves it as is.
		b.reg.remove<comp::Lifetime>(id);  // NORMAL_LIFE: persistent again
		auto [motion, pos] = b.reg.get<comp::Motion, comp::Position>(id);
		motion.velocity.zero();
		applyFacingMask(b, id, pos.facing, *sp->spec);
		b.reg.remove<comp::WarpingIn>(id);
	}
}

}  // namespace

// cleanup_dead_ship (tactrans.c:307-337): when the wreck finishes burning,
// everything the dead ship still owns -- in-flight nukes, the flame stream --
// goes with it. The C excepts drifting crew, which does not exist yet.
void sweepDeadShipOrdnance(Battle &b, EntityId id) noexcept
{
	// Every live entity has an Allegiance (uniform attach), so requiring it
	// in eachOrdered<Allegiance> is exact -- no null check needed.
	b.eachOrdered<comp::Allegiance>([&b, id](EntityId other,
											comp::Allegiance &a) {
		if (other == id || !(a.owner == id))
			return;
		b.reg.emplace_or_replace<comp::Lifetime>(other, comp::Lifetime{0});
		b.reg.emplace_or_replace<comp::Doomed>(other);
		b.reg.remove<comp::Collider>(other);
	});
}

void startShipExplosion(Battle &b, EntityId id) noexcept
{
	if (!b.alive(id))
		return;

	// The ship becomes its own explosion rather than vanishing
	// (tactrans.c:703-728): stops dead, loses energy, stops colliding, and
	// burns for a fixed number of frames.
	b.reg.get<comp::Motion>(id).velocity.zero();
	if (comp::ShipState *sp = b.ship(id))
		sp->energy = 0;
	// A dying ship draws ByFacing (Draw.cpp), not ByFrame, so there is no
	// AnimFrame to reset.
	b.reg.remove<comp::Doomed>(id);
	b.reg.emplace<comp::Lifetime>(id, comp::Lifetime{comp::Exploding::kLife});
	b.reg.remove<comp::Collider>(id);
	if (!b.reg.all_of<comp::SweepsOwnedOnDeath>(id))
		b.reg.emplace<comp::SweepsOwnedOnDeath>(id);
	if (!b.reg.all_of<comp::Exploding>(id))
		b.reg.emplace<comp::Exploding>(id);
}

namespace {

void explosionStep(Battle &b, EntityId id) noexcept
{
	if (!b.alive(id))
		return;

	// How many sparks this frame: the C's schedule (tactrans.c:545-575) ramps
	// 1/3/1 over the 26 frames it spawns for, then nothing for the last ten
	// while thrown sparks finish burning.
	const i32 age = ageOf(b, id, comp::Exploding::kLife);
	int count = 3;
	if (age <= 2 || (age >= 20 && age <= 25))
		count = 1;
	else if ((age >= 3 && age <= 5) || age == 18 || age == 19)
		count = 2;
	if (age > 25)
	{
		b.reg.remove<comp::Exploding>(id);
		return;
	}

	const Vec2i from = b.reg.get<comp::Position>(id).current;
	for (int n = 0; n < count; ++n)
	{
		// Scattered around the hull: random bearing, up to 8 display pixels
		// out, a third thrown 8 further so the cloud has an edge, not a rim
		// (tactrans.c:597-604).
		const u32 r0 = b.rng().next();
		const Angle spot{static_cast<int>(r0 >> 16)};
		i32 dist = displayToWorld(static_cast<i32>(r0 % 8u));
		if (((r0 >> 8) & 0xFFu) < 256u / 3u)
			dist += displayToWorld(8);

		// Drifting: its own bearing, up to 4 display pixels a frame. The speed
		// slice is HIBYTE(LOWORD()) -- one byte, then modulo
		// (tactrans.c:607-612); slicing different bits draws a different value
		// from the same stream.
		const u32 r1 = b.rng().next();
		const Angle drift{static_cast<int>(r1)};
		const i32 speed =
				displayToWorld(static_cast<i32>(((r1 >> 8) & 0xFFu) % 5u));

		// NEUTRAL: wreckage belongs to nobody -- cmd.allegiance below
		// defaults to exactly that.
		comp::Position dPos;
		dPos.current = wrap(
				Vec2i{from.x + cosine(spot, dist), from.y + sine(spot, dist)});
		dPos.next = dPos.current;
		// From components at the full 64-angle resolution, the way the C's
		// SetVelocityComponents call is (tactrans.c:607-612). Rounding the
		// drift to 16 facings visibly banded the cloud.
		comp::Motion dMotion;
		dMotion.velocity.setComponents(cosine(drift, worldToVelocity(speed)),
				sine(drift, worldToVelocity(speed)));

		b.queueSpawn(SpawnCommand{
				.layer = Layer::Background,
				.position = dPos,
				.motion = dMotion,
				// Never collidable, but the one decoration that drifts, so it
				// needs Motion where the others don't.
				.effect = true,
				.effectMoves = true,
				.debris = true,
				.lifetime = comp::Lifetime{comp::Debris::kLife},
		});
	}
}

}  // namespace

}  // namespace uqm::sim
