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

	// The engine's entire handling of SPECIAL (ship.c:342-343). Everything a
	// special actually *does* is per-ship code -- which is the asymmetry that
	// explains most of ships/.
	if (s.specialCounter > 0)
		--s.specialCounter;
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

		const Vec2i to = useNext ? t->next : t->current;
		const Vec2i d = wrapDelta(Vec2i{to.x - from.x, to.y - from.y});
		const int want = angleToFacing(arctan(d.x, d.y));
		const int delta = normalizeFacing(want - facing);
		if (!found || delta < bestDelta)
		{
			bestDelta = delta;
			found = true;
		}
	}

	if (!found || bestDelta == 0)
		return 0;

	// One step, the short way round. A guided missile turns at a rate; it does
	// not snap onto its target.
	facing = normalizeFacing(
			facing + (bestDelta <= kNumFacings / 2 ? 1 : -1));
	return 1;
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

		// The laser is a one-frame line in the C. Its only effect on the
		// simulation is the damage, so that is what is reproduced here.
		doDamage(*t, 1);

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
