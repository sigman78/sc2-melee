// Copyright the Ur-Quan Masters contributors. GPL-2.0-or-later.

#ifndef UQM2_SIM_DAMAGE_HPP
#define UQM2_SIM_DAMAGE_HPP

#include "engine/core/Types.hpp"
#include "sim/Element.hpp"

#define comp

namespace uqm::sim {

class Battle;
struct ShipState;

// Who loses what when things touch. do_damage (misc.c:204-225), DeltaCrew
// (status.c:333-370), weapon_collision (weapon.c:135-190) and collision
// (ship.c:352-377).

// Ship crew damage accumulated this frame, from every source, before one
// summed application and death check at the sync point. `lastFrom` is not
// yet consumed by anything.
comp struct DamageIncoming
{
	i32 amount = 0;
	EntityId lastFrom = kNoEntity;
};

// A weapon's damage. `blastOffset` positions the impact blast; `lingersOnHit`
// (ilwrath.c:141-148) undoes the death mark on impact so the flame still
// draws for the frame it died on -- every other weapon leaves it false.
comp struct Warhead
{
	i32 damage = 0;
	i32 blastOffset = 0;
	bool lingersOnHit = false;
};

// Applies a crew change and reports whether the ship survived it. False means
// this reduced the crew to zero -- which is the *only* thing the caller acts
// on, so it is the return value rather than a count.
bool deltaCrew(ShipState &s, i32 delta) noexcept;

// Hurts whatever this is. A gravity mass takes no damage (misc.c:214, asked
// without gravity.c's `+1`). A crewed hull's damage stacks in DamageIncoming
// instead, applied once at the sync point (Battle::step); `from` is who dealt
// it.
void doDamage(
		Battle &b, EntityId id, i32 damage, EntityId from = kNoEntity) noexcept;

// A weapon's collision response, dispatched on has<Warhead>. `id` is the
// weapon, `otherId` what it hit.
void weaponCollision(Battle &b, EntityId id, EntityId targetId) noexcept;

// A ship's, an asteroid's, and a planet's collision response -- only does
// anything when the other thing is a gravity mass; flying into a planet
// costs a point. The dispatch default: anything lacking a Warhead lands here.
void solidCollision(Battle &b, EntityId id, EntityId otherId) noexcept;

}  // namespace uqm::sim

#undef comp

#endif  // UQM2_SIM_DAMAGE_HPP
