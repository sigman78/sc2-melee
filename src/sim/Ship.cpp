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
			w.mask = d.weaponMask;
			w.onCollision = weaponCollision;
			w.flags = ElementFlags::FiniteLife;
			if (sp.ignoreSimilar)
				w.flags |= ElementFlags::IgnoreSimilar;
			w.velocity.setVector(sp.speed, sp.facing);

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
		return d;
	}();
	return data;
}

}  // namespace uqm::sim
