// Copyright the Ur-Quan Masters contributors. GPL-2.0-or-later.

#ifndef UQM2_SIM_DAMAGE_HPP
#define UQM2_SIM_DAMAGE_HPP

#include "sim/Element.hpp"

#include <cstdint>

namespace uqm::sim {

class Battle;

// Who loses what when things touch. do_damage (misc.c:204-225), DeltaCrew
// (status.c:333-370), weapon_collision (weapon.c:135-190) and collision
// (ship.c:352-377).

// The blast an expended weapon leaves. The C derives its life from the sprite's
// frame count (weapon.c:215-217), which does not exist in the simulation, so it
// is a constant here and the renderer will pick a frame from the remaining
// life rather than the other way round.
inline constexpr std::int32_t kBlastLife = 5;

// Applies a crew change and reports whether the ship survived it. False means
// this reduced the crew to zero -- which is the *only* thing the caller acts
// on, so it is the return value rather than a count.
bool deltaCrew(Element &e, std::int32_t delta) noexcept;

// Hurts whatever this is, by whatever rule applies to it.
//
// Three cases, and the third is the interesting one: a gravity mass takes no
// damage at all. That is asked *without* gravity.c's `+ 1` (misc.c:214), so a
// planet is immune and a ship fleeing at mass 100 is not -- see the note in
// Element.hpp on why the two predicates differ.
void doDamage(Element &e, std::int32_t damage) noexcept;

// A weapon's collision hook. `id` is the weapon; it reads its own
// `collidedWith` for the target.
void weaponCollision(Battle &b, EntityId id) noexcept;

// A ship's, an asteroid's, and a planet's collision hook -- one function in
// the C, and it only does anything when the *other* thing is a gravity mass.
// Flying into a planet costs a point.
void solidCollision(Battle &b, EntityId id) noexcept;

}  // namespace uqm::sim

#endif  // UQM2_SIM_DAMAGE_HPP
