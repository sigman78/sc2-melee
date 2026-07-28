// Copyright the Ur-Quan Masters contributors. GPL-2.0-or-later.

#include "Ilwrath.hpp"

#include "engine/core/Types.hpp"
#include "sim/Battle.hpp"
#include "sim/Damage.hpp"
#include "sim/ShipSystems.hpp"
#include "sim/Targeting.hpp"
#include "sim/Trig.hpp"
#include "sim/Velocity.hpp"

#include <cassert>

namespace uqm::sim {

usize
spawnAvengerPrimary(const ShipView &ship, std::span<Spawn> out) noexcept
{
	assert(!out.empty() && "spawn buffer must have room");

	Spawn &s = out[0];
	s.position = muzzlePosition(ship);
	s.facing = ship.facing;
	s.speed = ship.weaponSpeed;
	s.life = ship.weaponLife;
	s.damage = ship.weaponDamage;
	s.hitPoints = ship.weaponHitPoints;
	s.blastOffset = ship.blastOffset;
	s.playerNr = ship.playerNr;

	// ilwrath.c:200-201 -- `face = ShipFacing` but `index = 0`. The flame is
	// not a directional sprite, so unlike the Cruiser its frame does not
	// track the facing. Easy to "tidy" into symmetry and wrong if you do.
	s.frameIndex = 0;

	// ilwrath.c:203 -- IGNORE_SIMILAR. A stream of flame must not collide
	// with itself.
	s.ignoreSimilar = true;

	// ilwrath.c:219-222 -- the flame rides the Avenger's own velocity rather
	// than leaving it behind, so the stream trails the ship instead of hanging
	// in the space it just left.
	s.inheritsVelocity = true;
	return 1;
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
	{
		if (Collider *c = b.find<Collider>(id))
			c->mask = &ws->masks[static_cast<usize>(e->colorCycle)
					% ws->masks.size()];
	}
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

	applyFacingMask(b, id, *e, *b.ship(id)->spec);
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
