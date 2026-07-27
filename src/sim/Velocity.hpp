// Copyright the Ur-Quan Masters contributors. GPL-2.0-or-later.

#ifndef UQM2_SIM_VELOCITY_HPP
#define UQM2_SIM_VELOCITY_HPP

#include "engine/core/Geometry.hpp"
#include "sim/Trig.hpp"

#include <cstdint>

namespace uqm::sim {

// How a ship's speed stands against its own maximum. Lives here rather than
// in Thrust.hpp because an Element carries one and should not have to drag in
// the thrust machinery to do so (docs/cpp-conventions.md rule 2 on the
// engine's error/state surfaces staying small).
enum class SpeedState : std::uint8_t
{
	// Under the ship's own maximum: normal acceleration applies.
	Normal,
	// At it. Thrusting along the travel vector does nothing more; thrusting
	// across it turns the vector without adding speed.
	AtMax,
	// Above it -- only reachable through a gravity well, and only
	// deceleration is allowed until it drops back.
	BeyondMax,
};

// velocity.c, reproduced. Everything here is fixed point: a velocity
// component is in 1/32 world units (VELOCITY_SHIFT is 5), and the sub-unit
// remainder is carried frame to frame in a Bresenham error term so that a
// slow drift still moves exactly one unit every N frames rather than
// rounding to zero forever.
//
// The encoding is peculiar and is kept because the arithmetic depends on it:
// the *sign* of a component lives in a packed byte pair rather than in
// `vector`. For a positive component `incr` is (lo=1, hi=0); for a negative
// one it is (lo=0xFF, hi=(fract << 1)), and reconstruction is
//
//     component = (vector << 5) + (fract - hi(incr))
//
// which recovers -x for the negative case because the doubled fraction in
// `hi` cancels `fract` and then subtracts it again. `lo(incr)` is +1 or -1 as
// a signed byte and is what carries the accumulated fraction into whole units.
//
// Rewriting this as a plain signed fixed-point pair would be tidier and would
// change trajectories: the carry, the truncation and the sign handling
// interact, and ships are steered by integrating these every frame.

inline constexpr int kVelocityShift = 5;
inline constexpr std::int32_t kVelocityScale = 1 << kVelocityShift;  // 32

[[nodiscard]] constexpr std::int32_t
velocityToWorld(std::int32_t v) noexcept
{
	return v >> kVelocityShift;
}

[[nodiscard]] constexpr std::int32_t
worldToVelocity(std::int32_t l) noexcept
{
	return l << kVelocityShift;
}

class Velocity
{
public:
	constexpr Velocity() = default;

	// The travel angle, 0..63, or kFullCircle when the velocity is zero --
	// ARCTAN's sentinel, preserved (see Trig.hpp).
	[[nodiscard]] constexpr int travelAngle() const noexcept
	{
		return travelAngle_;
	}

	[[nodiscard]] constexpr bool isZero() const noexcept
	{
		return vector_ == Vec2i{} && incr_ == Vec2i{} && fract_ == Vec2i{};
	}

	// The current components, in velocity units.
	[[nodiscard]] constexpr Vec2i current() const noexcept
	{
		return Vec2i{worldToVelocity(vector_.x) + (fract_.x - hi(incr_.x)),
			worldToVelocity(vector_.y) + (fract_.y - hi(incr_.y))};
	}

	// Advance by `frames` and return the world-unit displacement, carrying
	// the sub-unit remainder into the error term. Mutating: the error is
	// state, which is why a "how far would this travel" query and an actual
	// step are the same call in the C and stay one here.
	Vec2i advance(int frames) noexcept;

	// Set from a magnitude in world units and a *facing* (0..15).
	void setVector(std::int32_t magnitude, int facing) noexcept;

	// Set from components in velocity units.
	void setComponents(std::int32_t dx, std::int32_t dy) noexcept;

	// Add to the current components.
	void deltaComponents(std::int32_t dx, std::int32_t dy) noexcept;

	constexpr void zero() noexcept { *this = Velocity{}; }

private:
	static constexpr std::int32_t hi(std::int32_t packed) noexcept
	{
		return (packed >> 8) & 0xFF;
	}
	// lo(incr) as a signed byte: +1 or -1.
	static constexpr std::int32_t loSigned(std::int32_t packed) noexcept
	{
		return static_cast<std::int8_t>(packed & 0xFF);
	}
	static constexpr std::int32_t makeWord(
			std::int32_t lo, std::int32_t hiByte) noexcept
	{
		return (lo & 0xFF) | ((hiByte & 0xFF) << 8);
	}
	static constexpr std::int32_t remainder(std::int32_t v) noexcept
	{
		return v & (kVelocityScale - 1);
	}

	void setAxis(std::int32_t v, std::int32_t &vector, std::int32_t &incr,
			std::int32_t &fract) noexcept;

	Vec2i vector_;   // whole world units per frame
	Vec2i fract_;    // sub-unit remainder, 0..31
	Vec2i error_;    // accumulated remainder
	Vec2i incr_;     // packed (sign, doubled fraction)
	int travelAngle_ = kFullCircle;
};

}  // namespace uqm::sim

#endif  // UQM2_SIM_VELOCITY_HPP
