// Copyright the Ur-Quan Masters contributors. GPL-2.0-or-later.

#ifndef UQM2_SIM_COLLISION_HPP
#define UQM2_SIM_COLLISION_HPP

#include "engine/core/Geometry.hpp"

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace uqm::sim {

// Swept per-pixel time-of-impact collision: intersec.c, rewritten.
//
// Open question 2 in docs/game-rewrite-plan.md, answered the way the plan
// recommends -- keep the semantics, rewrite only the expression. This is the
// most distinctive mechanic in the game and the most easily lost, and the
// AI's lookahead (cyborg.c:259) is built on it, so "close enough" is not.
//
// Two things change, both deliberate:
//
// 1. **No graphics context.** intersec.c:245 opens with
//    `if (!ContextActive () || max_time_val == 0) return 0;` -- collision
//    silently reports "no hit" when there is no renderer. Combined with
//    weapon.c:274-286, which rejection-loops until DrawablesIntersect
//    succeeds, that is why a naive headless build hangs instead of producing
//    wrong numbers. Here collision is pure sim over mask bits and cannot
//    depend on a renderer existing.
//
// 2. **Masks, not canvases.** The per-pixel test (canvas.c:2047-2056) reduces
//    to "are both pixels non-transparent", where transparent means alpha 0 or
//    equal to the colour key. That is one bit per pixel. The sim wants that
//    bit, not an SDL surface.

using TimeValue = std::uint16_t;

// A frame is divided into 256 sub-steps, and the walk runs t in 1..257.
inline constexpr int kTimeShift = 8;
inline constexpr TimeValue kMaxTimeValue = (1 << kTimeShift) + 1;  // 257

// A frame's 1-bit opacity, plus the hotspot that positions it.
//
// This is the "frame identity, not just masks" the plan asks sim/ to carve
// out: the hotspot is part of the collision contract, since a body's position
// is its hotspot and the mask hangs off it.
//
// Move-only: masks are per-frame and there are thousands of them
// (docs/cpp-conventions.md rule 6).
class CollisionMask
{
public:
	CollisionMask() = default;

	CollisionMask(const CollisionMask &) = delete;
	CollisionMask &operator=(const CollisionMask &) = delete;
	CollisionMask(CollisionMask &&) noexcept = default;
	CollisionMask &operator=(CollisionMask &&) noexcept = default;

	// `opaque` is row-major, one byte per pixel, non-zero where the pixel
	// would collide. A byte rather than a bool because that is the shape the
	// data arrives in -- decoded alpha, or index-vs-transparent-index -- and
	// because std::vector<bool> is a proxy type that cannot form a span.
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
	const CollisionMask *mask = nullptr;
	Vec2i from;
	Vec2i to;
};

// Where and when two bodies first touch.
struct Impact
{
	// 0 means no impact. Otherwise the sub-frame time the C returns, which
	// callers compare and order by -- earliest impact wins.
	TimeValue time = 0;

	// Hotspot positions one sub-step BEFORE the overlap -- the last clear
	// positions, which is what the C writes back through EndPoint
	// (intersec.c:218-229 updates the rects only after a failed test). The
	// caller places bodies touching, not interpenetrating. The one exception
	// is a pair already overlapping at time 1, where there is no clear
	// position to report and the start positions come back instead.
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
