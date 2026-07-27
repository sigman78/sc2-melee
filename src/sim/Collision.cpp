// Copyright the Ur-Quan Masters contributors. GPL-2.0-or-later.

#include "Collision.hpp"

#include <algorithm>
#include <cassert>
#include <cstddef>

namespace uqm::sim {

CollisionMask::CollisionMask(
		Extent2u size, Vec2i hotspot, std::span<const std::uint8_t> opaque)
	: size_(size), hotspot_(hotspot)
{
	assert(opaque.size() == size.area() && "mask size does not match extent");
	words_.assign((opaque.size() + 63) / 64, 0);
	for (std::size_t i = 0; i < opaque.size(); ++i)
	{
		if (opaque[i] != 0)
			words_[i >> 6] |= std::uint64_t{1} << (i & 63);
	}
}

namespace {

// A body's axis-aligned box at a given corner. The corner is the hotspot
// position minus the mask's hotspot, which is what intersec.c:263-270 does.
struct Box
{
	Vec2i corner;
	Extent2u extent;
};

[[nodiscard]] constexpr bool
boxIntersect(const Box &a, const Box &b, Box &out) noexcept
{
	const std::int32_t x0 = std::max(a.corner.x, b.corner.x);
	const std::int32_t y0 = std::max(a.corner.y, b.corner.y);
	const std::int32_t x1 = std::min(a.corner.x + static_cast<std::int32_t>(a.extent.w),
			b.corner.x + static_cast<std::int32_t>(b.extent.w));
	const std::int32_t y1 = std::min(a.corner.y + static_cast<std::int32_t>(a.extent.h),
			b.corner.y + static_cast<std::int32_t>(b.extent.h));
	if (x0 >= x1 || y0 >= y1)
		return false;

	out.corner = Vec2i{x0, y0};
	out.extent = Extent2u{static_cast<std::uint32_t>(x1 - x0),
		static_cast<std::uint32_t>(y1 - y0)};
	return true;
}

// canvas.c:2047-2056, minus the surface machinery: both pixels opaque
// anywhere in the overlap is a hit.
[[nodiscard]] bool
masksOverlap(const CollisionMask &m0, Vec2i c0, const CollisionMask &m1,
		Vec2i c1, const Box &inter) noexcept
{
	for (std::uint32_t y = 0; y < inter.extent.h; ++y)
	{
		for (std::uint32_t x = 0; x < inter.extent.w; ++x)
		{
			const std::int32_t px = inter.corner.x + static_cast<std::int32_t>(x);
			const std::int32_t py = inter.corner.y + static_cast<std::int32_t>(y);
			if (m0.opaqueAt(px - c0.x, py - c0.y)
					&& m1.opaqueAt(px - c1.x, py - c1.y))
				return true;
		}
	}
	return false;
}

// One body's Bresenham state for the walk.
struct Walk
{
	Vec2i corner;      // current top-left
	std::int32_t dx = 0, dy = 0;       // |delta|
	std::int32_t xincr = 0, yincr = 0;
	std::int32_t cycle = 0;            // max(|dx|, |dy|)
	std::int32_t xerror = 0, yerror = 0;
	std::int32_t timeError = 0;

	void
	init(Vec2i start, Vec2i delta) noexcept
	{
		corner = start;
		dx = delta.x;
		dy = delta.y;
		xincr = dx >= 0 ? 1 : -1;
		if (dx < 0)
			dx = -dx;
		yincr = dy >= 0 ? 1 : -1;
		if (dy < 0)
			dy = -dy;
		cycle = dx >= dy ? dx : dy;
		xerror = yerror = cycle;
	}

	// Fast-forward to just before time `t0`, as intersec.c:116-139 does.
	void
	seek(TimeValue t0) noexcept
	{
		std::int32_t start =
				cycle * static_cast<std::int32_t>(t0 - 1);
		timeError = start & ((1 << kTimeShift) - 1);
		start >>= kTimeShift;
		if (start <= 0)
			return;

		if (const std::int64_t e =
						static_cast<std::int64_t>(xerror) - std::int64_t{dx} * start;
				e > 0)
			xerror = static_cast<std::int32_t>(e);
		else
		{
			const std::int32_t delta =
					static_cast<std::int32_t>(-(e / cycle)) + 1;
			corner.x += xincr * delta;
			xerror = static_cast<std::int32_t>(e + std::int64_t{cycle} * delta);
		}

		if (const std::int64_t e =
						static_cast<std::int64_t>(yerror) - std::int64_t{dy} * start;
				e > 0)
			yerror = static_cast<std::int32_t>(e);
		else
		{
			const std::int32_t delta =
					static_cast<std::int32_t>(-(e / cycle)) + 1;
			corner.y += yincr * delta;
			yerror = static_cast<std::int32_t>(e + std::int64_t{cycle} * delta);
		}
	}

	// Advance one time unit; true when the body moved a pixel.
	bool
	step() noexcept
	{
		timeError += cycle;
		if (timeError < (1 << kTimeShift))
			return false;

		if ((xerror -= dx) <= 0)
		{
			corner.x += xincr;
			xerror += cycle;
		}
		if ((yerror -= dy) <= 0)
		{
			corner.y += yincr;
			yerror += cycle;
		}
		timeError -= (1 << kTimeShift);
		return true;
	}
};

}  // namespace

Impact
sweptIntersect(const Body &b0, const Body &b1, TimeValue maxTime)
{
	Impact none;

	// The C bails on a missing frame (intersec.c:261, 267). A missing mask is
	// a caller bug rather than a content one, but the sim must not read
	// through it either way.
	if (b0.mask == nullptr || b1.mask == nullptr || maxTime == 0)
		return none;
	if (b0.mask->empty() || b1.mask->empty())
		return none;

	maxTime = std::min(maxTime, kMaxTimeValue);

	const Extent2u e0 = b0.mask->size();
	const Extent2u e1 = b1.mask->size();

	// Corners, in the space the boxes live in.
	Vec2i c0 = b0.from - b0.mask->hotspot();
	Vec2i c1 = b1.from - b1.mask->hotspot();
	const Vec2i d0 = b0.to - b0.from;
	const Vec2i d1 = b1.to - b1.from;

	// The conservative time window (intersec.c:272-390). Each axis gives the
	// span of relative displacement over which the boxes can overlap; the
	// intersection of the two spans, scaled into time, bounds the search.
	std::int32_t dy = c1.y - c0.y;
	std::int32_t timeY0 = dy - static_cast<std::int32_t>(e0.h) + 1;
	std::int32_t timeY1 = dy + static_cast<std::int32_t>(e1.h) - 1;
	dy = d0.y - d1.y;

	const bool yPossible = (timeY0 <= 0 && timeY1 >= 0)
			|| (timeY0 > 0 && dy >= timeY0) || (timeY1 < 0 && dy <= timeY1);
	if (!yPossible)
		return none;

	std::int32_t dx = c1.x - c0.x;
	std::int32_t timeX0 = dx - static_cast<std::int32_t>(e0.w) + 1;
	std::int32_t timeX1 = dx + static_cast<std::int32_t>(e1.w) - 1;
	dx = d0.x - d1.x;

	const bool xPossible = (timeX0 <= 0 && timeX1 >= 0)
			|| (timeX0 > 0 && dx >= timeX0) || (timeX1 < 0 && dx <= timeX1);
	if (!xPossible)
		return none;

	if (dx == 0 && dy == 0)
	{
		timeY0 = timeY1 = 0;
	}
	else
	{
		if (timeY1 < 0)
		{
			const std::int32_t t = timeY0;
			timeY0 = -timeY1;
			timeY1 = -t;
		}
		else if (timeY0 <= 0)
		{
			if (dy < 0)
				timeY1 = -timeY0;
			timeY0 = 0;
		}
		if (dy < 0)
			dy = -dy;
		if (dy < timeY1)
			timeY1 = dy;
		// The C widens the window by one on each side "just to be safe".
		--timeY0;
		++timeY1;

		if (timeX1 < 0)
		{
			const std::int32_t t = timeX0;
			timeX0 = -timeX1;
			timeX1 = -t;
		}
		else if (timeX0 <= 0)
		{
			if (dx < 0)
				timeX1 = -timeX0;
			timeX0 = 0;
		}
		if (dx < 0)
			dx = -dx;
		if (dx < timeX1)
			timeX1 = dx;
		--timeX0;
		++timeX1;

		std::int64_t timeBeg, timeEnd, fract;
		if (dx == 0)
		{
			timeBeg = timeY0;
			timeEnd = timeY1;
			fract = dy;
		}
		else if (dy == 0)
		{
			timeBeg = timeX0;
			timeEnd = timeX1;
			fract = dx;
		}
		else
		{
			const std::int64_t bx = std::int64_t{timeX0} * dy;
			const std::int64_t by = std::int64_t{timeY0} * dx;
			timeBeg = bx < by ? by : bx;

			const std::int64_t ex = std::int64_t{timeX1} * dy;
			const std::int64_t ey = std::int64_t{timeY1} * dx;
			// Note: the C takes the *lesser* here despite the `>` test, which
			// is what bounds the window to the tighter axis.
			timeEnd = ex > ey ? ey : ex;

			fract = std::int64_t{dx} * dy;
		}

		timeBeg <<= kTimeShift;
		timeY0 = timeBeg < fract ? 0 : static_cast<std::int32_t>(timeBeg / fract);

		if (timeEnd >= fract
				|| (timeEnd <<= kTimeShift) >= fract * std::int64_t{maxTime})
			timeY1 = maxTime - 1;
		else
			timeY1 = static_cast<std::int32_t>((timeEnd + fract - 1) / fract) - 1;
	}

	if (timeY0 > timeY1)
		return none;

	// The walk (intersec.c:33-234).
	auto t0 = static_cast<TimeValue>(timeY0);
	const auto t1 = static_cast<TimeValue>(timeY1);

	Walk w0;
	Walk w1;
	w0.init(c0, d0);
	w1.init(c1, d1);

	const auto hit = [&](TimeValue t) -> Impact {
		return Impact{t, w0.corner + b0.mask->hotspot(),
			w1.corner + b1.mask->hotspot()};
	};

	const auto overlapping = [&]() -> bool {
		Box inter;
		const Box box0{w0.corner, e0};
		const Box box1{w1.corner, e1};
		return boxIntersect(box0, box1, inter)
				&& masksOverlap(*b0.mask, w0.corner, *b1.mask, w1.corner, inter);
	};

	if (t0 <= 1)
	{
		w0.timeError = w1.timeError = 0;
		if (t0 == 0)
		{
			// The C's `goto CheckFirstIntersection`: test the starting
			// positions before any motion.
			++t0;
			if (overlapping())
				return hit(t0);
		}
	}
	else
	{
		w0.seek(t0);
		w1.seek(t0);
	}

	while (t0 <= t1)
	{
		++t0;
		const bool moved0 = w0.step();
		const bool moved1 = w1.step();
		if ((moved0 || moved1) && overlapping())
			return hit(t0);
	}

	return none;
}

}  // namespace uqm::sim
