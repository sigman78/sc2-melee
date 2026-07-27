// Copyright the Ur-Quan Masters contributors. GPL-2.0-or-later.

#ifndef UQM2_SIM_IMPULSE_HPP
#define UQM2_SIM_IMPULSE_HPP

#include "sim/Element.hpp"
#include "sim/Thrust.hpp"

#include <cstdint>

namespace uqm::sim {

// Collision response: collide() from collide.c, and engine primitive #2.
//
// Momentum exchange along the impact axis, scaled by how head-on the
// collision was. Two details in the C are load-bearing and easy to lose:
//
//   - a glancing hit is *promoted to head-on* (collide.c:54-62). If the
//     relative travel is within a quadrant of the impact axis the shapes only
//     scraped, and leaving it alone means they scrape again next frame, and
//     the frame after. The fudge is what stops two ships grinding along each
//     other forever.
//   - two elements that did not move before colliding get DefyPhysics rather
//     than an impulse (collide.c:72-92), because there is no relative motion
//     to exchange. If they *both* already defy physics, their velocities are
//     zeroed and the impact angles are skewed by an octant, which is what
//     eventually pushes two stuck objects apart.

inline constexpr std::int32_t kCollisionTurnWait = 1;    // collide.h:28
inline constexpr std::int32_t kCollisionThrustWait = 3;  // collide.h:29

// Planets are gravity masses and do not take an impulse -- they push, they
// are not pushed. isGravityMass is in Element.hpp, next to the gravity.c
// variant it must not be confused with.

// Deterministic integer square root. UQM's square_root (libs/math/sqrt.c) is
// a bit-restoring routine, and it computes exactly floor(sqrt(v)) -- verified
// across ~4M values including the top of the 32-bit range. So the *result* is
// what has to match, not the algorithm. Integer rather than std::sqrt because
// sim/ has to give the same answer on every target, including wasm.
[[nodiscard]] std::uint32_t isqrt(std::uint32_t value) noexcept;

// The speed state implied by a velocity, rather than a flag someone patched.
//
// This is what the plan means by "derived from |v| vs max_thrust": with it
// available, chmmr.c:398-409, druuge.c:266 and mmrnmhrm.c:436-450 -- all of
// which hand-set these flags -- have nothing to hand-set.
//
// NOTE: applyImpulse still *resets* the state the way collide.c:109-110 does,
// rather than leaving callers to re-derive it. Switching to derivation is a
// one-line change at the call site and a deliberate behaviour change, because
// the C's flags can be stale by a frame. Kept separate so the switch is
// visible rather than smuggled in with the port.
[[nodiscard]] SpeedState deriveSpeedState(
		const Velocity &v, const ThrustProfile &profile) noexcept;

// Applies the collision response to both elements. They are assumed to have
// already been placed at their impact positions by the sweep.
void applyImpulse(Element &a, Element &b) noexcept;

}  // namespace uqm::sim

#endif  // UQM2_SIM_IMPULSE_HPP
