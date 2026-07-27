// Copyright the Ur-Quan Masters contributors. GPL-2.0-or-later.

#ifndef UQM2_SIM_THRUST_HPP
#define UQM2_SIM_THRUST_HPP

#include "sim/Velocity.hpp"
#include "sim/World.hpp"

#include <cstdint>

namespace uqm::sim {

// inertial_thrust (ship.c:55-147): facing and speed state are explicit
// args, not read from STARSHIP-global state -- no save/restore dance for
// ships that thrust off their own facing (Supox, supox.c:242-271).

// SpeedState is in Velocity.hpp -- the plan's primitive #2, flags derived
// from |v| against max_thrust rather than hand-patched by each ship
// (chmmr.c:398-409, druuge.c:266, mmrnmhrm.c:436-450 all patch them today).

struct ThrustProfile
{
	// Both in world units per frame.
	std::int32_t max = 0;
	std::int32_t increment = 0;

	// thrust_increment == max_thrust is the Arilou Skiff: it reaches full
	// speed in one frame and has no inertia at all. The C tests for this
	// equality rather than carrying a flag, and so does this.
	[[nodiscard]] constexpr bool inertialess() const noexcept
	{
		return increment == max;
	}
};

// Everything the thrust step needs to know about the ship's current state,
// and everything it reports back. No globals, no STARSHIP.
struct ThrustState
{
	SpeedState speed = SpeedState::Normal;
	bool inGravityWell = false;
};

// The hard ceiling a gravity whip can throw a ship to: 18 display pixels a
// frame (ship.c:58). Above the ship's own max but below this, a gravity well
// is allowed to keep accelerating it.
inline constexpr std::int32_t kMaxAllowedSpeed =
		worldToVelocity(displayToWorld(18));
inline constexpr std::int64_t kMaxAllowedSpeedSqr =
		std::int64_t{kMaxAllowedSpeed} * kMaxAllowedSpeed;

// Applies one frame of thrust along `facing`, returning the new speed state.
// `velocity` is updated in place.
[[nodiscard]] SpeedState thrust(Velocity &velocity, Facing facing,
		const ThrustProfile &profile, ThrustState state) noexcept;

}  // namespace uqm::sim

#endif  // UQM2_SIM_THRUST_HPP
