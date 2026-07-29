// Copyright the Ur-Quan Masters contributors. GPL-2.0-or-later.

#include "Specials.hpp"

#include "engine/core/Types.hpp"
#include "sim/Battle.hpp"
#include "sim/Damage.hpp"
#include "sim/ShipSystems.hpp"
#include "sim/Targeting.hpp"
#include "sim/Trig.hpp"
#include "sim/Velocity.hpp"
#include "sim/World.hpp"

namespace uqm::sim {

// LOOK_AHEAD (ilwrath.c:37): how many frames of both velocities the cloaked
// auto-aim leads the target by.
constexpr int kCloakAimLookAhead = 4;

namespace {

// The ambush snap (ilwrath.c:281-342): firing from full black aims at
// where the nearest enemy will be. TrackShip picks the target; the facing
// it steps is thrown away and recomputed from a four-frame lead.
void cloakedAutoAim(Battle &b, EntityId id) noexcept
{
	if (!b.alive(id))
		return;
	comp::Position *pos = &b.reg.get<comp::Position>(id);

	Facing facing = pos->facing;
	EntityId targetId = kNoEntity;
	if (trackShip(b, id, facing, &targetId) < 0)
		return;

	if (!b.alive(targetId) || !b.alive(id))
		return;
	pos = &b.reg.get<comp::Position>(id);
	const comp::Position &targetPos = b.reg.get<comp::Position>(targetId);

	// GetNextVelocityComponents on *copies* (ilwrath.c:292-296): the lead is
	// a question, not a step, and must not disturb either error accumulator.
	Velocity tv = b.reg.get<comp::Motion>(targetId).velocity;
	Velocity ov = b.reg.get<comp::Motion>(id).velocity;
	const Vec2i dT = tv.advance(kCloakAimLookAhead);
	const Vec2i dO = ov.advance(kCloakAimLookAhead);

	// Raw deltas, no WRAP_DELTA -- the C computes these unwrapped
	// (ilwrath.c:297-300), so an ambush across the seam aims the long way
	// round. Faithful, not an oversight.
	const i32 dx = (targetPos.current.x + dT.x) - (pos->current.x + dO.x);
	const i32 dy = (targetPos.current.y + dT.y) - (pos->current.y + dO.y);

	pos->facing = Angle(arctan(dx, dy)).facing();

	// And the ship may not immediately turn away from its own snap
	// (ilwrath.c:335-336).
	comp::ShipState *s = b.ship(id);
	if (s->turnWait == 0)
		s->turnWait = 1;

	applyFacingMask(b, id, pos->facing, *s->spec);
}

// The cloak machine (ilwrath.c:232-394): walks Cloak::level, keeps the
// Cloaked tag in sync with it, and snaps the ambush shot. Sole writer of
// both, which is why every other reader just asks has<Cloaked>.
void cloakStep(Battle &b, EntityId id) noexcept
{
	if (!b.alive(id))
		return;
	comp::ShipState *sp = b.ship(id);
	if (sp == nullptr)
		return;

	comp::ShipState &s = *sp;
	const ShipSpec &spec = *s.spec;
	const comp::Input &in = b.reg.get<comp::Input>(id);

	// ilwrath_preprocess (ilwrath.c:232-394): Cloak::level stands in for the
	// prim type/colour, its walk direction derived fresh each frame.
	// OBJECT_CLOAKED is the Cloaked tag, kept in sync with the level at this
	// function's end.
	comp::Cloak &c = b.reg.get<comp::Cloak>(id);

	// The C masks SPECIAL out of a *local* flags copy when an uncloak step
	// runs (ilwrath.c:346), which suppresses the activation block below for
	// that frame only. A local mirrors that exactly.
	bool specialMasked = false;

	if (c.level > 0)  // the prim is STAMPFILL: the machine is engaged
	{
		const bool weaponDischarge = any(in.buttons & ShipInput::Weapon)
				&& s.energy >= spec.weapon.energyCost;

		if (weaponDischarge
				|| (s.specialCounter == 0
						&& (any(in.buttons & ShipInput::Special)
								|| c.level < comp::Cloak::kFullLevel)))
		{
			// One step toward visible (ilwrath.c:250-348). Firing is the only
			// trigger that works mid-debounce; a key press needs the counter
			// spent, so an interrupted ramp keeps unwinding out on its own.
			if (c.level == comp::Cloak::kFullLevel && weaponDischarge)
			{
				// Stepping off BLACK under fire is the ambush.
				cloakedAutoAim(b, id);
				if (!b.alive(id))
					return;
			}
			--c.level;  // reaching 0 is the C's SetPrimType(STAMP)

			// Every uncloak step zeroes the debounce (ilwrath.c:347): re-cloak
			// is available the moment the ship is solid again, and SPECIAL is
			// masked this frame so the same press can't also activate below.
			s.specialCounter = 0;
			specialMasked = true;
		}
		else if (c.level < comp::Cloak::kFullLevel)
		{
			// One step toward black (ilwrath.c:349-374). At black, nothing:
			// the ship stays hidden until something above fires.
			++c.level;
		}
	}

	// Activation (ilwrath.c:377-393): SPECIAL with the debounce spent, paying
	// energy every time -- no free toggle-off, no half-price re-cloak. Restarts
	// at white even from mid-fade, though only reachable from solid in
	// practice.
	if (!specialMasked && any(in.buttons & ShipInput::Special)
			&& s.specialCounter == 0
			&& deltaEnergy(s, -spec.special.energyCost))
	{
		c.level = 1;  // WHITE, the walk's first colour
		s.specialCounter = spec.special.wait;
	}

	// The Cloaked tag is this machine's own invariant to keep: it is the
	// sole writer of `level`, so every other reader just asks has<Cloaked>
	// instead of re-deriving OBJECT_CLOAKED itself.
	if (c.level == comp::Cloak::kFullLevel)
		b.reg.emplace_or_replace<comp::Cloaked>(id);
	else
		b.reg.remove<comp::Cloaked>(id);
}

// Point defence (human.c:203-236): burns every shot in range, paying once
// for the volley. Reads PointDefence::range.
void pointDefenceStep(Battle &b, EntityId id) noexcept
{
	const comp::Allegiance *shipAllegiance =
			b.reg.try_get<comp::Allegiance>(id);
	if (shipAllegiance == nullptr)
		return;
	comp::ShipState *sp = b.ship(id);
	if (sp == nullptr)
		return;

	const ShipSpec &spec = *sp->spec;
	const i32 range = b.reg.get<comp::PointDefence>(id).range;
	if (range <= 0)
		return;

	const Vec2i from = b.reg.get<comp::Position>(id).next;
	bool paid = false;
	bool cannotAfford = false;

	// Every shot in range, not just the nearest: the C walks the whole list
	// and fires at each, paying once for the volley (human.c:225-236) -- a
	// Cruiser surrounded by fire clears all of it, or none if it can't afford
	// it.
	b.eachOrdered<comp::Physique, comp::Position>(
			[&](EntityId other, comp::Physique &otherPhys,
					comp::Position &otherPos) {
				if (cannotAfford || shipAllegiance == nullptr || other == id)
					return;

				if (!b.collidable(other))
					return;
				if (b.reg.all_of<comp::Cloaked>(other))
					return;  // human.c:203-204

				// No ownership test -- the C has none (human.c:203-204): the
				// Cruiser pays for and shoots down its OWN in-flight nukes in
				// range, a real tactical constraint.

				// A deliberate divergence from the C, which will fire on a
				// planet that just absorbs it (do_damage exempts gravity
				// masses).
				if (isGravityMass(otherPhys.mass))
					return;

				const Vec2i tNext = otherPos.next;
				const Vec2i dv = wrapDelta(tNext - from);
				const i32 dx = worldToDisplay(dv.x < 0 ? -dv.x : dv.x);
				const i32 dy = worldToDisplay(dv.y < 0 ? -dv.y : dv.y);
				if (dx > range || dy > range
						|| dx * dx + dy * dy > range * range)
					return;

				if (!paid)
				{
					if (!deltaEnergy(*sp, -spec.special.energyCost))
					{
						cannotAfford = true;  // cannot afford it, nothing burns
						return;
					}
					sp->specialCounter = spec.special.wait;
					paid = true;
				}

				doDamage(b, other, 1, id);

				// The beam is decorative -- only the damage above is real,
				// deterministic geometry. LASER_LIFE is 1 (weapon.c:52);
				// Beam{from,to} carries the ends, not motion -- this entity
				// has no Position.
				const Vec2i beamTo = tNext;

				// In the walk at the sync point: it draws its one frame of
				// life the step after this one, one frame later than the C's
				// same-step catch-up gave it.
				b.spawnBeam(Layer::Ordnance, comp::Beam{from, beamTo},
						 comp::Allegiance{shipAllegiance->playerNr, id})
						.with(comp::Lifetime{1});

				shipAllegiance = b.reg.try_get<comp::Allegiance>(id);
			});
}

}  // namespace

// The two slots, and the whole of this file's surface: component presence is
// the selector, so no ship is named here.
//
// The guard set is ShipMachines' three early returns as a query -- WarpingIn
// and Appearing are presence filters, crew == 0 a value test. Ordering
// against that pass is load-bearing; Specials.hpp says why.
void preTurnSpecialsPass(Battle &b) noexcept
{
	b.eachOrdered<comp::Cloak, comp::ShipState>(
			entt::exclude<comp::WarpingIn, comp::Appearing>,
			[&b](EntityId id, comp::Cloak &, comp::ShipState &s) {
				if (s.crew == 0)
					return;
				cloakStep(b, id);
			});
}

void runGatedSpecials(Battle &b, EntityId id) noexcept
{
	if (b.reg.all_of<comp::PointDefence>(id))
		pointDefenceStep(b, id);
}

}  // namespace uqm::sim
