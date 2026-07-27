// Copyright the Ur-Quan Masters contributors. GPL-2.0-or-later.

#ifndef UQM2_SIM_GRAVITY_HPP
#define UQM2_SIM_GRAVITY_HPP

#include "sim/EntityList.hpp"

#include <cstdint>

namespace uqm::sim {

class Battle;

// gravity.c, reproduced.
//
// Gravity in this game is not a force law. It is a fixed one-world-unit-per
// -frame nudge toward the source, applied to everything inside a disc, with no
// falloff at all -- the commented-out DIFUSE_GRAVITY block at gravity.c:96-111
// is the falloff that was tried and abandoned. What makes a gravity whip work
// is not the strength of the pull but the licence it grants: while a ship is
// in a well it may accelerate past its own maximum (ship.c:106-112).

// GRAVITY_THRESHOLD (element.h:199), in *display* pixels -- the comparison
// happens after WORLD_TO_DISPLAY, so the disc is 255 pixels, or 1020 world
// units.
inline constexpr std::int32_t kGravityRadius = 255;

// kMaxShipMass, kGravityMass, isGravityMass and isGravitySource are in
// Element.hpp. The last two are the same C macro asked with and without the
// `+ 1` gravity.c applies, and the difference is what makes a fleeing ship
// immune to a planet -- see the comment there.

// Runs the whole list against `id`.
//
// The two directions are one function in the C and stay one here. If `id` is a
// gravity source it pulls every non-source in range and returns false. If it
// is not, it returns whether it is inside someone else's well -- which is how
// spawn placement rejects a position (misc.c:63-70, ship.c:480) and how a
// ship learns it may exceed max speed.
bool calculateGravity(Battle &b, EntityId id);

// The planet's postprocess hook (misc.c:34-38).
void planetPostProcess(Battle &b, EntityId id) noexcept;

}  // namespace uqm::sim

#endif  // UQM2_SIM_GRAVITY_HPP
