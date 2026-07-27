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
bool
deltaEnergy(ShipState &s, std::int32_t delta) noexcept
{
	if (delta < 0 && s.energy + delta < 0)
		return false;

	s.energy += delta;
	if (s.energy > s.data->maxEnergy)
		s.energy = s.data->maxEnergy;
	if (s.energy < 0)
		s.energy = 0;
	return true;
}

// The silhouette follows the facing. In the C this is implicit -- the
// intersect frame is whatever frame is being displayed (process.c:159-160,
// InitIntersectFrame) -- so a ship that turns is immediately colliding as its
// new shape. Here the coupling is explicit, because the mask lives in the
// simulation and the sprite does not.
void
applyFacingMask(Element &e, const ShipData &d) noexcept
{
	if (d.facingMasks.empty())
		return;
	const std::size_t i =
			static_cast<std::size_t>(e.facing) % d.facingMasks.size();
	e.mask = &d.facingMasks[i];
}

}  // namespace

void
shipPreProcess(Battle &b, EntityId id) noexcept
{
	auto e = b.get(id);
	if (e == nullptr || e->ship.data == nullptr)
		return;

	ShipState &s = e->ship;
	const ShipData &d = *s.data;

	if (any(e->flags & ElementFlags::Appearing))
	{
		// First frame: the crew and energy come from the descriptor
		// (ship.c:169). Input is deliberately *not* latched on the appearing
		// frame, so a key held during the countdown does not fire on frame 1.
		s.crew = d.maxCrew;
		s.energy = d.maxEnergy;
		s.input = ShipInput::None;
		e->owner = id;  // a ship is its own pParent
		applyFacingMask(*e, d);
		return;
	}

	// The cloak ramp (ilwrath.c:239-285). One step a frame toward invisible
	// while the special is running, and back toward solid otherwise --
	// including while the trigger is held, because firing gives you away. The
	// C expresses this as a chain of colour comparisons on the fill primitive;
	// here it is the index into that same sequence.
	{
		// Direction comes from the special counter, not from the Cloaked flag.
		// Reading the flag here instead deadlocks: the ramp would only wind
		// down once the flag was clear, and the flag only clears once the ramp
		// reaches zero, so a cloaked ship stays cloaked forever.
		const bool firing = any(s.input & ShipInput::Weapon)
				&& s.energy >= d.weaponEnergyCost;
		const bool hiding = s.specialCounter > 0 && !firing;

		if (hiding && s.cloakLevel < kCloakSteps - 1)
			++s.cloakLevel;
		else if (!hiding && s.cloakLevel > 0)
			--s.cloakLevel;

		// Untargetable for as long as anything of the ship is faded. Partly
		// cloaked is still cloaked, which is what makes firing costly: the
		// ramp has to walk all the way back before you are hidden again.
		if (s.cloakLevel > 0)
			e->flags |= ElementFlags::Cloaked;
		else
			e->flags &= ~ElementFlags::Cloaked;
	}

	// Energy regeneration, gated by its own counter (ship.c:225-230).
	if (s.energyCounter > 0)
	{
		--s.energyCounter;
	}
	else if (s.energy < d.maxEnergy || d.energyRegen < 0)
	{
		(void)deltaEnergy(s, d.energyRegen);
		s.energyCounter = d.energyWait;
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
		e->turnWait = d.turnWait;
		applyFacingMask(*e, d);
	}

	// Thrust (ship.c:256-276). The facing is passed in rather than read off a
	// global -- see Thrust.hpp.
	if (e->thrustWait > 0)
	{
		--e->thrustWait;
	}
	else if (any(s.input & ShipInput::Thrust))
	{
		s.speed = thrust(e->velocity, e->facing, d.thrust,
				ThrustState{s.speed, s.inGravityWell});
		// ship.c:263-267 clears the whole speed/gravity group and ORs in what
		// inertial_thrust returned. Gravity re-sets the well flag next frame
		// if the ship is still in one, so a ship that leaves the well loses
		// its licence to exceed max speed on the very next thrust.
		s.inGravityWell = false;
		e->thrustWait = d.thrustWait;

		// Exhaust, only on the frames the ship actually accelerates
		// (ship.c:274) -- not on every frame the key is held.
		spawnIonTrail(b, id);
	}
}

void
shipPostProcess(Battle &b, EntityId id) noexcept
{
	auto e = b.get(id);
	if (e == nullptr || e->ship.data == nullptr)
		return;

	ShipState &s = e->ship;
	const ShipData &d = *s.data;

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
			&& deltaEnergy(s, -d.weaponEnergyCost))
	{
		ShipView view;
		view.position = e->next;
		view.velocity = e->velocity;
		view.facing = e->facing;
		view.playerNr = e->playerNr;
		view.weaponSpeed = d.weaponSpeed;
		view.weaponLife = d.weaponLife;
		view.weaponDamage = d.weaponDamage;
		view.weaponHitPoints = d.weaponHitPoints;
		view.muzzleOffset = d.muzzleOffset;
		view.blastOffset = d.blastOffset;

		SpawnBuffer buf{};
		const std::size_t n =
				d.spawnPrimary != nullptr ? d.spawnPrimary(view, buf) : 0;

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
			w.mass = 0;
			w.blastOffset = sp.blastOffset;
			w.mask = d.weaponMasks.empty()
					? nullptr
					: &d.weaponMasks[static_cast<std::size_t>(sp.facing)
							% d.weaponMasks.size()];
			w.onCollision = weaponCollision;
			w.preProcess = sp.preProcess != nullptr ? sp.preProcess
													: d.weaponPreProcess;
			// The shot reads its own guidance parameters from the descriptor
			// that fired it, so it carries the pointer. It is not a ship and
			// has no ShipState otherwise.
			w.ship.data = &d;
			w.flags = ElementFlags::FiniteLife;
			if (sp.ignoreSimilar)
				w.flags |= ElementFlags::IgnoreSimilar;
			w.velocity.setVector(sp.speed, sp.facing);
			w.owner = id;  // pParent: what IGNORE_SIMILAR is tested against

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

		s.weaponCounter = d.weaponWait;
	}

	// SPECIAL. The engine's own part is only the counter (ship.c:342-343);
	// everything a special *does* is per-ship code, which is the asymmetry
	// that explains most of ships/. Hence a hook rather than a switch.
	if (s.specialCounter > 0)
	{
		--s.specialCounter;
		if (s.specialCounter == 0)
		{
			// The counter running out stops the ship *hiding*; the ramp then
			// walks it back to solid over the next few frames, and clearing
			// the flag is that ramp's job rather than this one's. Clearing it
			// here would snap a half-faded Avenger back to opaque.
		}
	}
	else if (any(s.input & ShipInput::Special) && d.special != nullptr)
	{
		d.special(b, id);
	}
}

int
trackShip(Battle &b, EntityId tracker, int &facing) noexcept
{
	const auto self = b.get(tracker);
	if (self == nullptr)
		return 0;

	// The C reads `next` once the tracker has been preprocessed and `current`
	// before it (weapon.c:356-368), which is the same distinction gravity.c
	// makes -- and for the same reason: half the list has already moved.
	const bool useNext = any(self->flags & ElementFlags::PreProcessed);
	const Vec2i from = useNext ? self->next : self->current;

	int bestDelta = 0;
	std::int32_t bestDistance = 0;
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
			found = true;
		}
	}

	if (!found || bestDelta == 0)
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
	if (e == nullptr || e->ship.data == nullptr)
		return;

	const ShipData &d = *e->ship.data;

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
		e->turnWait = d.weaponTrackWait;
	}

	// And accelerate as it goes (human.c:148-157): speed climbs with how much
	// of its life it has spent, capped. A nuke that has been chasing you for a
	// while is much harder to outrun than one just launched, which is most of
	// what makes the Cruiser feel like the Cruiser.
	std::int32_t speed = d.weaponSpeed
			+ (d.weaponLife - e->lifeSpan) * d.weaponThrustScale;
	if (speed > d.weaponMaxSpeed)
		speed = d.weaponMaxSpeed;
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
	if (e == nullptr || e->ship.data == nullptr)
		return;

	if (any(e->flags & ElementFlags::Appearing))
	{
		// Arriving: invisible, untouchable, and on a clock
		// (tactrans.c:858-866). The ship is in the simulation the whole time
		// -- it simply cannot be hit and is not drawn -- which is what stops
		// two ships materialising inside each other.
		e->ship.crew = e->ship.data->maxCrew;
		e->ship.energy = e->ship.data->maxEnergy;
		e->ship.input = ShipInput::None;
		e->owner = id;
		e->lifeSpan = kWarpInFrames;
		e->flags |= ElementFlags::NonSolid | ElementFlags::FiniteLife;
		e->velocity.zero();
		return;
	}

	// A shadow of itself each frame, fading through the same ramp the exhaust
	// uses -- which is exactly why the C shares cycle_ion_trail between them.
	spawnIonTrail(b, id);

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
		applyFacingMask(*e, *e->ship.data);
	}
}

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
	e.preProcess = nullptr;
	e.postProcess = nullptr;
}

void
cruiserSpecial(Battle &b, EntityId id) noexcept
{
	auto ship = b.get(id);
	if (ship == nullptr || ship->ship.data == nullptr)
		return;

	const ShipData &d = *ship->ship.data;
	const std::int32_t range = d.pointDefenceRange;
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
			continue;  // human.c:202
		if (t->playerNr == ship->playerNr)
			continue;  // its own fire is not a threat

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
			if (!deltaEnergy(ship->ship, -d.specialEnergyCost))
				return;  // cannot afford it, so nothing burns
			ship->ship.specialCounter = d.specialWait;
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
		b.spawnFront(std::move(beam));

		ship = b.get(id);
		if (ship == nullptr)
			return;
	}
}

void
avengerSpecial(Battle &b, EntityId id) noexcept
{
	auto ship = b.get(id);
	if (ship == nullptr || ship->ship.data == nullptr)
		return;

	const ShipData &d = *ship->ship.data;
	if (!deltaEnergy(ship->ship, -d.specialEnergyCost))
		return;

	// Cloak. It hides the Avenger from weapon tracking and from point
	// defence, and it lasts exactly as long as the counter -- there is no
	// second press to turn it off (ilwrath.c:377-393).
	ship->flags |= ElementFlags::Cloaked;
	ship->ship.specialCounter = d.specialWait;
	if (ship->ship.cloakLevel == 0)
		ship->ship.cloakLevel = 1;  // start the ramp
}

// --------------------------------------------------------------------------
// The two M1 ships.

const ShipData &
earthlingCruiser() noexcept
{
	// human.c:27-49. MAX_THRUST 24, THRUST_INCREMENT 3.
	static const ShipData data = [] {
		ShipData d;
		d.maxCrew = 18;
		d.maxEnergy = 18;
		d.energyRegen = 1;
		d.energyWait = 8;
		d.thrust = ThrustProfile{24, 3};
		d.thrustWait = 4;
		d.turnWait = 1;
		d.weaponWait = 10;
		d.weaponEnergyCost = 9;
		d.specialWait = 9;
		d.specialEnergyCost = 4;
		d.mass = 6;
		d.spawnPrimary = spawnCruiserPrimary;
		// MISSILE_SPEED is max(MAX_THRUST, DISPLAY_TO_WORLD(10)) == 40.
		d.weaponSpeed = 40;
		d.weaponLife = 60;
		d.weaponDamage = 4;
		d.weaponHitPoints = 1;
		d.muzzleOffset = 42;   // HUMAN_OFFSET
		d.blastOffset = 8;     // NUKE_OFFSET

		// The nuke is guided and accelerates: TRACK_WAIT 3,
		// MAX_MISSILE_SPEED = DISPLAY_TO_WORLD(20) == 80, THRUST_SCALE =
		// DISPLAY_TO_WORLD(1) == 4 (human.c:43-50).
		d.weaponTrackWait = 3;
		d.weaponMaxSpeed = 80;
		d.weaponThrustScale = 4;
		d.weaponPreProcess = nukePreProcess;
		d.special = cruiserSpecial;
		d.pointDefenceRange = 100;   // LASER_RANGE
		return d;
	}();
	return data;
}

const ShipData &
ilwrathAvenger() noexcept
{
	// ilwrath.c:28-49. Note THRUST_WAIT 0 and WEAPON_WAIT 0 -- the Avenger
	// accelerates every frame and its flame is continuous, which is why it
	// feels nothing like the Cruiser.
	static const ShipData data = [] {
		ShipData d;
		d.maxCrew = 22;
		d.maxEnergy = 16;
		d.energyRegen = 4;
		d.energyWait = 4;
		d.thrust = ThrustProfile{25, 5};
		d.thrustWait = 0;
		d.turnWait = 2;
		d.weaponWait = 0;
		d.weaponEnergyCost = 1;
		d.specialWait = 13;
		d.specialEnergyCost = 3;
		d.mass = 7;
		d.spawnPrimary = spawnAvengerPrimary;
		d.weaponSpeed = 25;    // MISSILE_SPEED == MAX_THRUST
		d.weaponLife = 8;
		d.weaponDamage = 1;
		d.weaponHitPoints = 1;
		d.muzzleOffset = 29;   // ILWRATH_OFFSET
		d.blastOffset = 0;     // MISSILE_OFFSET
		d.special = avengerSpecial;
		return d;
	}();
	return data;
}

}  // namespace uqm::sim
