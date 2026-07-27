// Copyright the Ur-Quan Masters contributors. GPL-2.0-or-later.

#ifndef UQM2_SIM_WORLD_HPP
#define UQM2_SIM_WORLD_HPP

#include "engine/core/Geometry.hpp"

#include <cstdint>

namespace uqm::sim {

// The battlefield's coordinate space, and its topology.
//
// This is open question 4 in docs/game-rewrite-plan.md, settled the cheap
// way. The plan worried that ScreenWidth/ScreenHeight are *runtime globals*
// feeding LOG_SPACE_WIDTH and WRAP_X/WRAP_Y -- the battlefield's topology,
// not merely where things spawn -- and called it not retrofittable.
//
// It turns out there is nothing to retrofit. `ScreenWidth = 320;
// ScreenHeight = 240;` at sdl2_pure.c:312-313 is the only assignment to
// either in the whole tree; --res moves ScreenWidthActual, which is the
// window. So the "runtime global" never varies, and making it a constant is
// behaviourally a no-op that removes a whole class of "does the arena change
// size at 4K" bug.
//
// If resolution independence is ever wanted, it has to be a deliberate change
// to *this* file with the melee baseline re-established afterwards -- not
// something that falls out of a window resize.

inline constexpr std::int32_t kScreenWidth = 320;
inline constexpr std::int32_t kScreenHeight = 240;
inline constexpr std::int32_t kStatusWidth = 64;   // units.h:39
inline constexpr std::int32_t kSafeX = 0;
inline constexpr std::int32_t kSafeY = 0;

// The visible play area, in display pixels (units.h:43-45).
inline constexpr std::int32_t kSpaceWidth =
		kScreenWidth - kStatusWidth - kSafeX * 2;   // 256
inline constexpr std::int32_t kSpaceHeight = kScreenHeight - kSafeY * 2;  // 240

// World units per display pixel. units.h:79-82: ONE_SHIFT is 2, so a pixel is
// 4 world units, and MAX_REDUCTION is 3, so the logical arena is 8x the
// visible one on each axis.
inline constexpr int kOneShift = 2;
inline constexpr std::int32_t kScaledOne = 1 << kOneShift;  // 4
inline constexpr int kMaxReduction = 3;

constexpr std::int32_t
displayToWorld(std::int32_t x) noexcept
{
	return x << kOneShift;
}
constexpr std::int32_t
worldToDisplay(std::int32_t x) noexcept
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

// The torus. 256 * 4 * 8 == 8192 by 240 * 4 * 8 == 7680.
inline constexpr std::int32_t kLogSpaceWidth =
		displayToWorld(kSpaceWidth) << kMaxReduction;
inline constexpr std::int32_t kLogSpaceHeight =
		displayToWorld(kSpaceHeight) << kMaxReduction;

static_assert(kLogSpaceWidth == 8192, "the arena width the C computes today");
static_assert(kLogSpaceHeight == 7680, "the arena height the C computes today");

// The battlefield wraps. WRAP_VAL (units.h:214-216) folds by one period only,
// which is all a single step can need since nothing moves a whole arena in a
// frame -- and asserting that is cheaper than a modulo on every axis of every
// entity every step.
constexpr std::int32_t
wrapX(std::int32_t x) noexcept
{
	if (x < 0)
		return x + kLogSpaceWidth;
	if (x >= kLogSpaceWidth)
		return x - kLogSpaceWidth;
	return x;
}

constexpr std::int32_t
wrapY(std::int32_t y) noexcept
{
	if (y < 0)
		return y + kLogSpaceHeight;
	if (y >= kLogSpaceHeight)
		return y - kLogSpaceHeight;
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
constexpr std::int32_t
wrapDeltaX(std::int32_t dx) noexcept
{
	if (dx < 0)
		return (-dx <= kLogSpaceWidth / 2) ? dx : kLogSpaceWidth + dx;
	return (dx <= kLogSpaceWidth / 2) ? dx : dx - kLogSpaceWidth;
}

constexpr std::int32_t
wrapDeltaY(std::int32_t dy) noexcept
{
	if (dy < 0)
		return (-dy <= kLogSpaceHeight / 2) ? dy : kLogSpaceHeight + dy;
	return (dy <= kLogSpaceHeight / 2) ? dy : dy - kLogSpaceHeight;
}

constexpr Vec2i
wrapDelta(Vec2i d) noexcept
{
	return Vec2i{wrapDeltaX(d.x), wrapDeltaY(d.y)};
}

}  // namespace uqm::sim

#endif  // UQM2_SIM_WORLD_HPP
