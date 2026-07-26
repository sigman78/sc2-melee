// Copyright the Ur-Quan Masters contributors. GPL-2.0-or-later.

#ifndef UQM2_SIM_TRIG_HPP
#define UQM2_SIM_TRIG_HPP

#include <array>
#include <cstdint>

namespace uqm::sim {

// The game's angle model and its trig tables (units.h:180-212, trans.c).
//
// A circle is 64 units, not 360 degrees and not radians. Facings are 16, so
// four angle units to a facing. Everything is integer: the tables are the
// definition of sine here, not an approximation of it, and the rewrite
// reproduces them exactly rather than calling std::sin -- ships turn in whole
// angle units and a one-count difference in a table entry is a different
// trajectory.
//
// The convention, which is worth stating because it is not the usual one:
//
//     angle 0  = up      (-y)      SINE(0,m) == -m,  COSINE(0,m) == 0
//     angle 16 = right   (+x)
//     angle 32 = down    (+y)
//     angle 48 = left    (-x)
//
// so angles increase clockwise from straight up, in screen coordinates. That
// falls out of the table being -cos rather than sin: sinetab[a] is
// -cos(2*pi*a/64), which is sin shifted by a quarter turn.

inline constexpr int kCircleShift = 6;
inline constexpr int kFullCircle = 1 << kCircleShift;   // 64
inline constexpr int kHalfCircle = kFullCircle >> 1;    // 32
inline constexpr int kQuadrant = kFullCircle >> 2;      // 16
inline constexpr int kOctant = kFullCircle >> 3;        // 8

inline constexpr int kFacingShift = 4;
inline constexpr int kNumFacings = 1 << kFacingShift;   // 16

inline constexpr int kSinShift = 14;
inline constexpr int kSinScale = 1 << kSinShift;        // 16384

// Transcribed from trans.c:23-89 by reading the file, not by hand. The values
// are (SIZE)(x * 16384) of the authored decimals, so they truncate toward
// zero and sit within 1 of -cos(2*pi*a/64) * 16384.
inline constexpr std::array<std::int16_t, kFullCircle> kSineTab{
	-16384, -16305, -16069, -15678, -15136, -14449, -13622, -12664,
	-11585, -10393, -9102, -7723, -6269, -4756, -3196, -1605,
	0, 1605, 3196, 4756, 6269, 7723, 9102, 10393,
	11585, 12664, 13622, 14449, 15136, 15678, 16069, 16305,
	16384, 16305, 16069, 15678, 15136, 14449, 13622, 12664,
	11585, 10393, 9102, 7723, 6269, 4756, 3196, 1605,
	0, -1605, -3196, -4756, -6269, -7723, -9102, -10393,
	-11585, -12664, -13622, -14449, -15136, -15678, -16069, -16305,
};

[[nodiscard]] constexpr int
normalizeAngle(int a) noexcept
{
	return a & (kFullCircle - 1);
}

[[nodiscard]] constexpr int
normalizeFacing(int f) noexcept
{
	return f & (kNumFacings - 1);
}

// ANGLE_TO_FACING rounds to the nearest facing (units.h:189); the +2 before
// the shift is the round-half-up.
[[nodiscard]] constexpr int
angleToFacing(int a) noexcept
{
	return (a + (1 << (kCircleShift - kFacingShift - 1)))
			>> (kCircleShift - kFacingShift);
}

[[nodiscard]] constexpr int
facingToAngle(int f) noexcept
{
	return f << (kCircleShift - kFacingShift);
}

[[nodiscard]] constexpr std::int16_t
sinVal(int a) noexcept
{
	return kSineTab[static_cast<std::size_t>(normalizeAngle(a))];
}

[[nodiscard]] constexpr std::int16_t
cosVal(int a) noexcept
{
	return sinVal(a + kQuadrant);
}

// SINE/COSINE scale a magnitude by the table entry. The intermediate is
// widened because a magnitude times 16384 leaves the 16 bits the C's SIZE
// would have; the C widens to long for the same reason.
[[nodiscard]] constexpr std::int32_t
sine(int a, std::int32_t m) noexcept
{
	return static_cast<std::int32_t>(
			(static_cast<std::int64_t>(sinVal(a)) * m) >> kSinShift);
}

[[nodiscard]] constexpr std::int32_t
cosine(int a, std::int32_t m) noexcept
{
	return sine(a + kQuadrant, m);
}

// The 33-entry quarter-turn table ARCTAN interpolates (trans.c:95-130).
inline constexpr std::array<std::uint8_t, 33> kArcTanTab{
	0, 0, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 6, 6, 6, 6, 7,
	7, 7, 7, 7, 7, 8, 8, 8,
};

// The angle of (dx, dy), 0..63.
//
// EXCEPT for the zero vector, where it returns kFullCircle -- 64, which is
// *not* a normalized angle. That is the C's behaviour (trans.c:134-135) and
// it is a sentinel meaning "no direction", so callers have to notice it. A
// caller that feeds it straight to sinVal() would read past the table; the
// assert in sinVal's normalizeAngle folds it to 0 instead, which is a
// direction, so the check belongs at the call site.
[[nodiscard]] constexpr int
arctan(int dx, int dy) noexcept
{
	if (dx == 0 && dy == 0)
		return kFullCircle;

	std::int32_t v1 = dx < 0 ? -dx : dx;
	const std::int32_t v2 = dy < 0 ? -dy : dy;

	// (32 * lesser + greater/2) / greater -- a rounded 0..32 index.
	if (v1 > v2)
	{
		const std::int32_t i =
				((v2 << (kCircleShift - 1)) + (v1 >> 1)) / v1;
		v1 = kQuadrant - kArcTanTab[static_cast<std::size_t>(i)];
	}
	else
	{
		const std::int32_t i =
				((v1 << (kCircleShift - 1)) + (v2 >> 1)) / v2;
		v1 = kArcTanTab[static_cast<std::size_t>(i)];
	}

	if (dx < 0)
		v1 = kFullCircle - v1;
	if (dy > 0)
		v1 = kHalfCircle - v1;

	return normalizeAngle(v1);
}

// --------------------------------------------------------------------------
// Golden vectors, from running the C.

static_assert(kSineTab.size() == 64);
static_assert(kSineTab[0] == -16384 && kSineTab[16] == 0
		&& kSineTab[32] == 16384 && kSineTab[48] == 0);

// The convention above, asserted rather than described.
static_assert(sine(0, 1000) == -1000, "angle 0 points up");
static_assert(cosine(0, 1000) == 0);
static_assert(cosine(kQuadrant, 1000) == 1000, "angle 16 points right");
static_assert(sine(kQuadrant, 1000) == 0);

// The zero vector's unnormalized sentinel.
static_assert(arctan(0, 0) == kFullCircle);

// Cardinals and diagonals.
static_assert(arctan(0, -1) == 0);
static_assert(arctan(1, 0) == 16);
static_assert(arctan(0, 1) == 32);
static_assert(arctan(-1, 0) == 48);
static_assert(arctan(1, -1) == 8);
static_assert(arctan(1, 1) == 24);
static_assert(arctan(-1, 1) == 40);
static_assert(arctan(-1, -1) == 56);

// Off-axis, and a magnitude that would overflow a 16-bit intermediate.
static_assert(arctan(100, 25) == 18);
static_assert(arctan(25, 100) == 30);
static_assert(arctan(-300, 700) == 36);
static_assert(arctan(32767, 1) == 16);

// Facings round to nearest, and round-trip at the facing boundaries.
static_assert(angleToFacing(0) == 0);
static_assert(angleToFacing(2) == 1, "half a facing rounds up");
static_assert(angleToFacing(4) == 1);
static_assert(facingToAngle(1) == 4);
static_assert(normalizeFacing(angleToFacing(kFullCircle)) == 0);

}  // namespace uqm::sim

#endif  // UQM2_SIM_TRIG_HPP
