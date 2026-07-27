// Copyright the Ur-Quan Masters contributors. GPL-2.0-or-later.

#include "Thrust.hpp"

#include "engine/core/Types.hpp"

namespace uqm::sim {

namespace {

[[nodiscard]] constexpr i64
speedSquared(Vec2i v) noexcept
{
	return i64{v.x} * v.x + i64{v.y} * v.y;
}

[[nodiscard]] constexpr bool
atOrBeyondMax(SpeedState s) noexcept
{
	return s == SpeedState::AtMax || s == SpeedState::BeyondMax;
}

}  // namespace

SpeedState
thrust(Velocity &velocity, Facing facing, const ThrustProfile &profile,
		ThrustState state) noexcept
{
	const int currentAngle = facing.angle().raw();
	const int travelAngle = velocity.travelAngle();

	// The Skiff: acceleration is instantaneous, so there is nothing to
	// integrate and no way to be beyond maximum.
	if (profile.inertialess())
	{
		velocity.setVector(profile.max, facing);
		return SpeedState::AtMax;
	}

	// Already flat out along this heading, and not being flung by a gravity
	// well: nothing further to add.
	if (travelAngle == currentAngle && atOrBeyondMax(state.speed)
			&& !state.inGravityWell)
		return state.speed;

	const i32 increment = worldToVelocity(profile.increment);
	const Vec2i current = velocity.current();
	const i64 currentSpeed = speedSquared(current);

	const Vec2i desired{current.x + cosine(currentAngle, increment),
		current.y + sine(currentAngle, increment)};
	const i64 desiredSpeed = speedSquared(desired);

	const i32 maxVel = worldToVelocity(profile.max);
	const i64 maxSpeed = i64{maxVel} * maxVel;

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
			velocity.setVector(profile.max, facing);
		return SpeedState::AtMax;
	}

	// Thrusting across the travel vector at max turns it without adding speed:
	// half an increment goes on along the new heading, a whole one comes off
	// the old, keeping the magnitude roughly constant while direction swings.
	Velocity turned = velocity;
	turned.deltaComponents(
			cosine(currentAngle, increment >> 1) - cosine(travelAngle, increment),
			sine(currentAngle, increment >> 1) - sine(travelAngle, increment));

	const i64 turnedSpeed = speedSquared(turned.current());
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
