// Copyright the Ur-Quan Masters contributors. GPL-2.0-or-later.

#ifndef UQM2_SIM_IMPULSE_HPP
#define UQM2_SIM_IMPULSE_HPP

#include "engine/core/Types.hpp"
#include "sim/Element.hpp"
#include "sim/Thrust.hpp"

namespace uqm::sim {

// Collision response: collide() from collide.c, engine primitive #2. Momentum
// exchange along the impact axis, scaled by how head-on the hit was.
//
// A glancing hit is promoted to head-on (collide.c:54-62), or two shapes
// scrape again next frame. Elements with no relative motion get DefyPhysics
// instead of an impulse (collide.c:72-92); if both already defy physics,
// velocities zero and the impact angle skews by an octant to separate them.

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

// Applies the collision response to both elements. They are assumed to have
// already been placed at their impact positions by the sweep.
// aIsShip/bIsShip: the PlayerShip trait is a tag component now (review-004
// X4), and pure physics takes facts as parameters rather than a registry.
// aScratch/bScratch: the collided/defyPhysics scratch, read and written here
// but owned by the collision domain, not by Impulse -- no registry access.
void applyImpulse(Element &a, bool aIsShip, CollisionScratch &aScratch,
		Element &b, bool bIsShip, CollisionScratch &bScratch) noexcept;

}  // namespace uqm::sim

#endif  // UQM2_SIM_IMPULSE_HPP
