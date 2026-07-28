// Copyright the Ur-Quan Masters contributors. GPL-2.0-or-later.

#ifndef UQM2_SIM_GRAVITY_HPP
#define UQM2_SIM_GRAVITY_HPP

#include "engine/core/Types.hpp"
#include "sim/Entity.hpp"

namespace uqm::sim {

class Battle;

// gravity.c, reproduced: a fixed one-world-unit-per-frame nudge toward the
// source over a disc, no falloff (gravity.c:96-111). In a well a ship may
// accelerate past its own maximum (ship.c:106-112).

// GRAVITY_THRESHOLD (element.h:199), in *display* pixels -- the comparison
// happens after WORLD_TO_DISPLAY, so the disc is 255 pixels, or 1020 world
// units.
inline constexpr i32 kGravityRadius = 255;

// kMaxShipMass, kGravityMass, isGravityMass, isGravitySource: Element.hpp.
// The two predicates are the same C macro with/without gravity.c's `+ 1`,
// which is what makes a fleeing ship immune to a planet.

// Runs the whole list against `id`. One function for both directions: if
// `id` is a source, pulls everything in range and returns false; otherwise
// returns whether `id` is in someone else's well (misc.c:63-70, ship.c:480).
bool calculateGravity(Battle &b, EntityId id);

// The pipeline's gravity pass (review-006 Z4, slot 6's companion: runs
// right after GuidedSteer, before Integrate): every collidable gravity
// source pulls whatever sits in its well. What planetPostProcess used to do
// once per planet through the retired per-element postProcess dispatch,
// now a dedicated whole-spine pass.
void gravityPass(Battle &b);

}  // namespace uqm::sim

#endif  // UQM2_SIM_GRAVITY_HPP
