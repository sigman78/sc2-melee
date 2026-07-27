// Copyright the Ur-Quan Masters contributors. GPL-2.0-or-later.

#ifndef UQM2_SIM_COLLISION_HPP
#define UQM2_SIM_COLLISION_HPP

#include "engine/core/Borrowed.hpp"
#include "engine/core/Geometry.hpp"

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace uqm::sim {

// Swept per-pixel time-of-impact collision: intersec.c, rewritten (per-pixel
// test canvas.c:2047-2056). AI lookahead depends on the semantics
// (cyborg.c:259). Masks, not canvases, headless-safe -- design-notes D2.

using TimeValue = std::uint16_t;

// A frame is divided into 256 sub-steps, and the walk runs t in 1..257.
inline constexpr int kTimeShift = 8;
inline constexpr TimeValue kMaxTimeValue = (1 << kTimeShift) + 1;  // 257

// A frame's 1-bit opacity plus the hotspot that positions it -- the hotspot
// is part of the collision contract, since a body's position is its hotspot.
// Move-only: masks are per-frame, there are thousands (rule 6).
class CollisionMask
{
public:
	CollisionMask() = default;

	CollisionMask(const CollisionMask &) = delete;
	CollisionMask &operator=(const CollisionMask &) = delete;
	CollisionMask(CollisionMask &&) noexcept = default;
	CollisionMask &operator=(CollisionMask &&) noexcept = default;

	// `opaque` is row-major, one byte per pixel, non-zero where the pixel would
	// collide: a byte, not bool, because that's the source data's shape and
	// std::vector<bool> can't form a span.
	CollisionMask(
			Extent2u size, Vec2i hotspot, std::span<const std::uint8_t> opaque);

	[[nodiscard]] Extent2u size() const noexcept { return size_; }
	[[nodiscard]] Vec2i hotspot() const noexcept { return hotspot_; }
	[[nodiscard]] bool empty() const noexcept { return size_.empty(); }

	[[nodiscard]] bool
	opaqueAt(std::int32_t x, std::int32_t y) const noexcept
	{
		if (x < 0 || y < 0 || x >= static_cast<std::int32_t>(size_.w)
				|| y >= static_cast<std::int32_t>(size_.h))
			return false;
		const std::size_t bit =
				static_cast<std::size_t>(y) * size_.w + static_cast<std::size_t>(x);
		return (words_[bit >> 6] >> (bit & 63)) & 1u;
	}

private:
	std::vector<std::uint64_t> words_;
	Extent2u size_;
	Vec2i hotspot_;
};

// A body moving from `from` to `to` over one frame. Positions are hotspot
// positions, in the same space the mask's hotspot is measured in.
struct Body
{
	Borrowed<const CollisionMask> mask = nullptr;
	Vec2i from;
	Vec2i to;
};

// Where and when two bodies first touch.
struct Impact
{
	// 0 means no impact. Otherwise the sub-frame time the C returns, which
	// callers compare and order by -- earliest impact wins.
	TimeValue time = 0;

	// Hotspot positions one sub-step before the overlap, matching the C's
	// EndPoint (intersec.c:218-229): bodies land touching, not interpenetrating.
	// Exception: already overlapping at time 1 returns the start positions.
	Vec2i at0;
	Vec2i at1;

	[[nodiscard]] constexpr explicit operator bool() const noexcept
	{
		return time != 0;
	}
};

// The first time in (0, maxTime] at which the two masks overlap by at least
// one pixel, or a falsy Impact.
[[nodiscard]] Impact sweptIntersect(
		const Body &b0, const Body &b1, TimeValue maxTime = kMaxTimeValue);

}  // namespace uqm::sim

#endif  // UQM2_SIM_COLLISION_HPP
