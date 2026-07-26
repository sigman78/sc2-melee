// Copyright the Ur-Quan Masters contributors. GPL-2.0-or-later.

#include "Thrust.hpp"

namespace uqm::sim {

namespace {

[[nodiscard]] constexpr std::int64_t
speedSquared(Vec2i v) noexcept
{
	return std::int64_t{v.x} * v.x + std::int64_t{v.y} * v.y;
}

[[nodiscard]] constexpr bool
atOrBeyondMax(SpeedState s) noexcept
{
	return s == SpeedState::AtMax || s == SpeedState::BeyondMax;
}

}  // namespace

SpeedState
thrust(Velocity &velocity, int facing, const ThrustProfile &profile,
		ThrustState state) noexcept
{
	const int currentAngle = facingToAngle(normalizeFacing(facing));
	const int travelAngle = velocity.travelAngle();

	// The Skiff: acceleration is instantaneous, so there is nothing to
	// integrate and no way to be beyond maximum.
	if (profile.inertialess())
	{
		velocity.setVector(profile.maxThrust, facing);
		return SpeedState::AtMax;
	}

	// Already flat out along this heading, and not being flung by a gravity
	// well: nothing further to add.
	if (travelAngle == currentAngle && atOrBeyondMax(state.speed)
			&& !state.inGravityWell)
		return state.speed;

	const std::int32_t increment = worldToVelocity(profile.thrustIncrement);
	const Vec2i current = velocity.current();
	const std::int64_t currentSpeed = speedSquared(current);

	const Vec2i desired{current.x + cosine(currentAngle, increment),
		current.y + sine(currentAngle, increment)};
	const std::int64_t desiredSpeed = speedSquared(desired);

	const std::int32_t maxVel = worldToVelocity(profile.maxThrust);
	const std::int64_t maxSpeed = std::int64_t{maxVel} * maxVel;

	if (desiredSpeed <= maxSpeed)
	{
		// Normal acceleration.
		velocity.setComponents(desired.x, desired.y);
		return SpeedState::Normal;
	}

	if ((state.inGravityWell && desiredSpeed <= kMaxAllowedSpeedSqr)
			|| desiredSpeed < currentSpeed)
	{
		// Being accelerated by a well, within the hard ceiling; or slowing
		// down after a whip. Both are allowed above the ship's own maximum.
		velocity.setComponents(desired.x, desired.y);
		return SpeedState::BeyondMax;
	}

	if (travelAngle == currentAngle)
	{
		// Max acceleration along the existing vector. Only snap to the
		// canonical max-speed vector when not already above it, so a whipped
		// ship is not silently slowed.
		if (currentSpeed <= maxSpeed)
			velocity.setVector(profile.maxThrust, facing);
		return SpeedState::AtMax;
	}

	// Thrusting across the travel vector at maximum: this turns the vector
	// without adding speed. Half an increment goes on along the new heading
	// and a whole one comes off along the old, which is what keeps the
	// magnitude roughly constant while the direction swings.
	Velocity turned = velocity;
	turned.deltaComponents(
			cosine(currentAngle, increment >> 1) - cosine(travelAngle, increment),
			sine(currentAngle, increment >> 1) - sine(travelAngle, increment));

	const std::int64_t turnedSpeed = speedSquared(turned.current());
	if (turnedSpeed > maxSpeed)
	{
		// The turn would have made it faster, not just differently aimed.
		// Keep it only if it is at least slower than before.
		if (turnedSpeed < currentSpeed)
			velocity = turned;
		return SpeedState::BeyondMax;
	}

	velocity = turned;
	return SpeedState::Normal;
}

}  // namespace uqm::sim
