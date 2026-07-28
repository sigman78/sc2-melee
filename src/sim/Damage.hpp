// Copyright the Ur-Quan Masters contributors. GPL-2.0-or-later.

#ifndef UQM2_SIM_DAMAGE_HPP
#define UQM2_SIM_DAMAGE_HPP

#include "engine/core/Types.hpp"
#include "sim/Element.hpp"

namespace uqm::sim {

class Battle;
struct ShipState;

// Who loses what when things touch. do_damage (misc.c:204-225), DeltaCrew
// (status.c:333-370), weapon_collision (weapon.c:135-190) and collision
// (ship.c:352-377).

// Blast life is a constant, not derived from sprite frames -- see
// design-notes V9.
inline constexpr i32 kBlastLife = 5;

// Ship crew damage accumulated this frame, from every source, before one
// summed application and one death check at the sync point -- review-006
// §2's second Z4 refinement: splitting a crewed hull's damage from the
// pair-local hit-point exchange (piercing) is what lets several hits the
// same frame stack explicitly instead of racing on whichever hook ran
// first. `lastFrom` is not yet consumed by anything; it is here so a future
// kill-attribution reader does not need another pass over doDamage's
// call sites.
struct DamageIncoming
{
	i32 amount = 0;
	EntityId lastFrom = kNoEntity;
};

// Applies a crew change and reports whether the ship survived it. False means
// this reduced the crew to zero -- which is the *only* thing the caller acts
// on, so it is the return value rather than a count.
bool deltaCrew(ShipState &s, i32 delta) noexcept;

// Hurts whatever this is, by whatever rule applies to it. A gravity mass
// takes no damage: asked *without* gravity.c's `+ 1` (misc.c:214) -- see
// Element.hpp for why the two predicates differ.
//
// A crewed hull does not lose crew here any more (Z4): the damage stacks in
// its DamageIncoming component instead, applied once at the sync point
// (Battle::step). `from`, defaulted to kNoEntity, is who dealt it.
void doDamage(Battle &b, EntityId id, i32 damage,
		EntityId from = kNoEntity) noexcept;

// A weapon's collision hook. `id` is the weapon; it reads its own
// `collidedWith` for the target.
void weaponCollision(Battle &b, EntityId id) noexcept;

// A ship's, an asteroid's, and a planet's collision hook -- one function in
// the C, and it only does anything when the *other* thing is a gravity mass.
// Flying into a planet costs a point.
void solidCollision(Battle &b, EntityId id) noexcept;

}  // namespace uqm::sim

#endif  // UQM2_SIM_DAMAGE_HPP
