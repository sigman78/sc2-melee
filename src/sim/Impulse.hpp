// Copyright the Ur-Quan Masters contributors. GPL-2.0-or-later.

#ifndef UQM2_SIM_IMPULSE_HPP
#define UQM2_SIM_IMPULSE_HPP

#include "engine/core/Types.hpp"
#include "sim/Element.hpp"
#include "sim/Ship.hpp"
#include "sim/Thrust.hpp"

namespace uqm::sim {

// Collision response: collide() from collide.c. Momentum exchange along the
// impact axis, scaled by how head-on the hit was. A glancing hit is promoted
// to head-on (collide.c:54-62), or two shapes scrape again next frame.
// Elements with no relative motion get DefyPhysics instead (collide.c:72-92);
// if both already defy physics, velocities zero and the angle skews an octant.

inline constexpr i32 kCollisionTurnWait = 1;    // collide.h:28
inline constexpr i32 kCollisionThrustWait = 3;  // collide.h:29

// Planets are gravity masses and do not take an impulse -- they push, they
// are not pushed. isGravityMass is in Element.hpp, next to the gravity.c
// variant it must not be confused with.

// Deterministic integer square root: matches UQM's square_root
// (libs/math/sqrt.c), floor(sqrt(v)), verified across ~4M values. Integer,
// not std::sqrt, so every target (incl. wasm) gives the same answer.
[[nodiscard]] u32 isqrt(u32 value) noexcept;

// The speed state implied by velocity vs max thrust, not a hand-set flag
// (chmmr.c:398-409, druuge.c:266, mmrnmhrm.c:436-450). applyImpulse resets
// it to Normal unconditionally (collide.c:104-110); the C's flags go stale.
[[nodiscard]] SpeedState deriveSpeedState(
		const Velocity &v, const ThrustProfile &profile) noexcept;

// Applies the collision response to both elements, assumed already placed at
// their impact positions by the sweep. aShip/bShip are non-null iff that side
// is a ship, for the turn/thrust stagger (collide.c:111-116).
void applyImpulse(const Position &aPos, Motion &aMotion,
		const Physique &aPhys, ShipState *aShip, CollisionScratch &aScratch,
		const Position &bPos, Motion &bMotion, const Physique &bPhys,
		ShipState *bShip, CollisionScratch &bScratch) noexcept;

}  // namespace uqm::sim

#endif  // UQM2_SIM_IMPULSE_HPP
