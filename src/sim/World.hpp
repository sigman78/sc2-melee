// Copyright the Ur-Quan Masters contributors. GPL-2.0-or-later.

#ifndef UQM2_SIM_WORLD_HPP
#define UQM2_SIM_WORLD_HPP

#include "engine/core/Geometry.hpp"
#include "engine/core/Types.hpp"

namespace uqm::sim {

// The battlefield's coordinate space and topology: ScreenWidth/ScreenHeight
// are compile-time constants, not runtime globals -- the only assignment is
// sdl2_pure.c:312-313, and --res moves only ScreenWidthActual (the window).

inline constexpr Extent2i kScreen{320, 240};
inline constexpr i32 kStatusWidth = 64;   // units.h:39

// The visible play area, in display pixels (units.h:43-45). No safe margin
// on this build -- kSafeX/kSafeY were both 0, subtracted twice per axis.
inline constexpr Extent2i kSpace{
		kScreen.w - kStatusWidth,   // 256
		kScreen.h};                 // 240

// World units per display pixel. units.h:79-82: ONE_SHIFT is 2, so a pixel is
// 4 world units, and MAX_REDUCTION is 3, so the logical arena is 8x the
// visible one on each axis.
inline constexpr int kOneShift = 2;
inline constexpr i32 kScaledOne = 1 << kOneShift;  // 4
inline constexpr int kMaxReduction = 3;

constexpr i32
displayToWorld(i32 x) noexcept
{
	return x << kOneShift;
}
constexpr i32
worldToDisplay(i32 x) noexcept
{
	return x >> kOneShift;
}

// Point conversions. Collision masks are measured in display pixels, so
// anything handed to the intersect test has to come through here first --
// that is what InitIntersectStartPoint/EndPoint do in the C (collide.h:44-54).
constexpr Vec2i
displayToWorld(Vec2i p) noexcept
{
	return Vec2i{displayToWorld(p.x), displayToWorld(p.y)};
}

constexpr Vec2i
worldToDisplay(Vec2i p) noexcept
{
	return Vec2i{worldToDisplay(p.x), worldToDisplay(p.y)};
}

constexpr Extent2i
displayToWorld(Extent2i e) noexcept
{
	return Extent2i{displayToWorld(e.w), displayToWorld(e.h)};
}

// The torus. 256 * 4 * 8 == 8192 by 240 * 4 * 8 == 7680.
inline constexpr Extent2i kArena = displayToWorld(kSpace) << kMaxReduction;

static_assert(kArena.w == 8192, "the arena width the C computes today");
static_assert(kArena.h == 7680, "the arena height the C computes today");

// The battlefield wraps. WRAP_VAL (units.h:214-216) folds by one period
// only -- enough since nothing moves a whole arena in a frame, and cheaper
// than a modulo every axis, every entity, every step.
constexpr i32
wrapX(i32 x) noexcept
{
	if (x < 0)
		return x + kArena.w;
	if (x >= kArena.w)
		return x - kArena.w;
	return x;
}

constexpr i32
wrapY(i32 y) noexcept
{
	if (y < 0)
		return y + kArena.h;
	if (y >= kArena.h)
		return y - kArena.h;
	return y;
}

constexpr Vec2i
wrap(Vec2i p) noexcept
{
	return Vec2i{wrapX(p.x), wrapY(p.y)};
}

// The shorter way round the torus, which is what "how far apart are these"
// means here (WRAP_DELTA_X, units.h:217-219). Distances across the seam are
// short, not enormous, and an AI that gets this wrong flies the long way.
constexpr i32
wrapDeltaX(i32 dx) noexcept
{
	if (dx < 0)
		return (-dx <= kArena.w / 2) ? dx : kArena.w + dx;
	return (dx <= kArena.w / 2) ? dx : dx - kArena.w;
}

constexpr i32
wrapDeltaY(i32 dy) noexcept
{
	if (dy < 0)
		return (-dy <= kArena.h / 2) ? dy : kArena.h + dy;
	return (dy <= kArena.h / 2) ? dy : dy - kArena.h;
}

constexpr Vec2i
wrapDelta(Vec2i d) noexcept
{
	return Vec2i{wrapDeltaX(d.x), wrapDeltaY(d.y)};
}

}  // namespace uqm::sim

#endif  // UQM2_SIM_WORLD_HPP
