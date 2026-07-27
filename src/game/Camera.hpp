// Copyright the Ur-Quan Masters contributors. GPL-2.0-or-later.

#ifndef UQM2_GAME_CAMERA_HPP
#define UQM2_GAME_CAMERA_HPP

#include "engine/core/Geometry.hpp"
#include "sim/World.hpp"

#include <cstdint>
#include <span>

namespace uqm::game {

// The melee camera: where the view is centred and how far out it is
// zoomed -- the `optMeleeScale` unification (design-notes.md D6).
// process.c:206-358 is CalcReduction/CalcView, the two forked C cameras.

enum class MeleeScale
{
	// TFB_SCALE_STEP: three discrete zoom levels with hysteresis so the view
	// does not oscillate at a boundary -- the 3DO behaviour, sharper since a
	// power-of-two reduction lands every sprite pixel on a screen pixel.
	Step,

	// The default in the C. A continuous zoom, smoother to watch and blurrier,
	// because sprites are then scaled by an arbitrary ratio.
	Continuous,
};

#ifndef UQM_MELEE_SCALE
#define UQM_MELEE_SCALE Continuous
#endif

inline constexpr MeleeScale kMeleeScale = MeleeScale::UQM_MELEE_SCALE;

// Zoom is a 1/256ths multiplier: 256 is 1:1, 1024 is four times out.
inline constexpr int kZoomShift = 8;                    // units.h:76
inline constexpr std::int32_t kZoomOne = 1 << kZoomShift;
inline constexpr int kMaxVisReduction = 2;              // units.h:72
inline constexpr int kReductionShift = 1;               // units.h:73

// MAX_ZOOM_OUT (units.h:77), which is 1 << (8 + 3 - 1) == 1024, i.e. 4:1.
inline constexpr std::int32_t kMaxZoomOut =
		1 << (kZoomShift + sim::kMaxReduction - 1);

// TRANSITION_WIDTH/HEIGHT (units.h:92-95): the arena at MAX_VIS_REDUCTION.
inline constexpr std::int32_t kTransitionWidth =
		sim::displayToWorld(sim::kSpaceWidth) << kMaxVisReduction;
inline constexpr std::int32_t kTransitionHeight =
		sim::displayToWorld(sim::kSpaceHeight) << kMaxVisReduction;

// Hysteresis on zooming back in (process.c:235-236), in world units.
inline constexpr std::int32_t kHysteresisX = sim::displayToWorld(24);
inline constexpr std::int32_t kHysteresisY = sim::displayToWorld(20);

class Camera
{
public:
	[[nodiscard]] Vec2i centre() const noexcept { return centre_; }
	[[nodiscard]] std::int32_t zoom() const noexcept { return zoom_; }

	// Recentres on the ships and picks a zoom that keeps them all on screen.
	//
	// process.c:670-697 folds each ship in by walking halfway toward it (the
	// origin lands on the midpoint for two ships); zoom uses the full
	// separation, not the half.
	void
	follow(std::span<const Vec2i> ships) noexcept
	{
		if (ships.empty())
			return;

		Vec2i origin = ships.front();
		std::int32_t spanX = 0;
		std::int32_t spanY = 0;

		for (const Vec2i &s : ships.subspan(1))
		{
			const Vec2i d = sim::wrapDelta(
					Vec2i{s.x - origin.x, s.y - origin.y});
			origin = Vec2i{origin.x + (d.x >> 1), origin.y + (d.y >> 1)};

			// Written out rather than std::max, to keep <algorithm> out of a
			// header this widely included for two comparisons.
			const std::int32_t adx = d.x < 0 ? -d.x : d.x;
			const std::int32_t ady = d.y < 0 ? -d.y : d.y;
			if (adx > spanX)
				spanX = adx;
			if (ady > spanY)
				spanY = ady;
		}

		centre_ = sim::wrap(origin);
		zoom_ = kMeleeScale == MeleeScale::Step ? stepZoom(spanX, spanY)
												: continuousZoom(spanX, spanY);
	}

	// World position to screen pixel, relative to the viewport's top left.
	[[nodiscard]] Vec2i
	toScreen(Vec2i world) const noexcept
	{
		const Vec2i d = sim::wrapDelta(
				Vec2i{world.x - centre_.x, world.y - centre_.y});
		return Vec2i{sim::kSpaceWidth / 2 + scale(d.x),
				sim::kSpaceHeight / 2 + scale(d.y)};
	}

	// A length in world units, in screen pixels. Sprites need this too, which
	// is the point of having one zoom rather than two camera modes.
	[[nodiscard]] std::int32_t
	scale(std::int32_t worldLength) const noexcept
	{
		// One rounded division, not world-to-display-then-divide-by-zoom:
		// that truncates twice (throws away two bits, then divides by up to
		// four more), which reads as jitter and breathes sprite sizes by a pixel.
		const std::int64_t denom = std::int64_t{sim::kScaledOne} * zoom_;
		const std::int64_t num = std::int64_t{worldLength} * kZoomOne;
		const std::int64_t half = denom / 2;
		return static_cast<std::int32_t>(
				(num >= 0 ? num + half : num - half) / denom);
	}

private:
	// The C aligns the origin to a display pixel here (DISPLAY_ALIGN,
	// process.c:337-342). Not done here: `scale` keeps full world precision,
	// so snapping the origin would judder the whole screen by a pixel instead.

	// process.c:222-243.
	[[nodiscard]] std::int32_t
	stepZoom(std::int32_t dx, std::int32_t dy) const noexcept
	{
		const std::int32_t sdx = dx;
		const std::int32_t sdy = dy;

		int reduction = kMaxVisReduction;
		while (reduction > 0)
		{
			dx <<= kReductionShift;
			dy <<= kReductionShift;
			if (dx > kTransitionWidth || dy > kTransitionHeight)
				break;
			reduction -= kReductionShift;
		}

		// Zooming back *in* needs the ships to have closed by more than the
		// hysteresis margin, or the view oscillates between two levels while
		// they circle each other at the boundary.
		const int current = currentReduction();
		if (reduction < current && current <= kMaxVisReduction)
		{
			const int shift = kMaxVisReduction - reduction;
			if (((sdx + kHysteresisX) << shift) > kTransitionWidth
					|| ((sdy + kHysteresisY) << shift) > kTransitionHeight)
				reduction += kReductionShift;
		}

		return kZoomOne << reduction;
	}

	// process.c:254-269.
	[[nodiscard]] static std::int32_t
	continuousZoom(std::int32_t dx, std::int32_t dy) noexcept
	{
		const auto axis = [](std::int32_t d, std::int32_t extent) {
			std::int32_t z = static_cast<std::int32_t>(
					std::int64_t{d} * kMaxZoomOut / (extent >> 2));
			if (z < kZoomOne)
				z = kZoomOne;
			else if (z > kMaxZoomOut)
				z = kMaxZoomOut;
			return z;
		};
		const std::int32_t zx = axis(dx, sim::kLogSpaceWidth);
		const std::int32_t zy = axis(dy, sim::kLogSpaceHeight);
		return zy > zx ? zy : zx;
	}

	[[nodiscard]] int
	currentReduction() const noexcept
	{
		int r = 0;
		for (std::int32_t z = zoom_; z > kZoomOne; z >>= 1)
			++r;
		return r;
	}

	Vec2i centre_{sim::kLogSpaceWidth / 2, sim::kLogSpaceHeight / 2};
	std::int32_t zoom_ = kZoomOne;
};

}  // namespace uqm::game

#endif  // UQM2_GAME_CAMERA_HPP
