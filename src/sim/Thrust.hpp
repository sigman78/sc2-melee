// Copyright the Ur-Quan Masters contributors. GPL-2.0-or-later.

#ifndef UQM2_SIM_THRUST_HPP
#define UQM2_SIM_THRUST_HPP

#include "sim/Velocity.hpp"
#include "sim/World.hpp"

#include <cstdint>

namespace uqm::sim {

// inertial_thrust (ship.c:55-147), with its two hidden inputs made explicit.
//
// This is engine primitive #1 from docs/game-rewrite-plan.md, and the reason
// it is a prerequisite rather than a nicety: the C reads the facing out of
// STARSHIP->ShipFacing and the speed state out of
// STARSHIP->cur_status_flags. A ship that wants to thrust in any direction
// other than the one it is pointing -- Supox's omni-thrust, Umgah's retro --
// therefore has to save ShipFacing, overwrite it, call in, hand-merge the
// returned flags and restore. That is sixty lines in supox.c:242-271 for
// twelve lines of intent, and every ship that does it re-imports the same
// shape.
//
// Here the facing is an argument and the speed state is a value in and a
// value out. Supox's special becomes: pick a facing delta, call thrust with
// it. Nothing to save, nothing to restore.

// SpeedState is in Velocity.hpp -- the plan's primitive #2, flags derived
// from |v| against max_thrust rather than hand-patched by each ship
// (chmmr.c:398-409, druuge.c:266, mmrnmhrm.c:436-450 all patch them today).

struct ThrustProfile
{
	// Both in world units per frame.
	std::int32_t maxThrust = 0;
	std::int32_t thrustIncrement = 0;

	// thrust_increment == max_thrust is the Arilou Skiff: it reaches full
	// speed in one frame and has no inertia at all. The C tests for this
	// equality rather than carrying a flag, and so does this.
	[[nodiscard]] constexpr bool inertialess() const noexcept
	{
		return thrustIncrement == maxThrust;
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
[[nodiscard]] SpeedState thrust(Velocity &velocity, int facing,
		const ThrustProfile &profile, ThrustState state) noexcept;

}  // namespace uqm::sim

#endif  // UQM2_SIM_THRUST_HPP
