// Copyright the Ur-Quan Masters contributors. GPL-2.0-or-later.

#ifndef UQM2_SIM_TRIG_HPP
#define UQM2_SIM_TRIG_HPP

#include "engine/core/Types.hpp"

#include <array>

namespace uqm::sim {

// The game's angle model (units.h:180-212, trans.c): a circle is 64 units,
// 16 facings; tables are integer and exact (not std::sin), since a
// one-count difference is a different trajectory.
//
//     angle 0=up(-y)  16=right(+x)  32=down(+y)  48=left(-x)
//
// Clockwise from up, because sinetab[a] is -cos(2*pi*a/64): sin shifted a
// quarter turn.

inline constexpr int kCircleShift = 6;
inline constexpr int kFullCircle = 1 << kCircleShift;  // 64
inline constexpr int kHalfCircle = kFullCircle >> 1;   // 32
inline constexpr int kQuadrant = kFullCircle >> 2;     // 16
inline constexpr int kOctant = kFullCircle >> 3;       // 8

inline constexpr int kFacingShift = 4;
inline constexpr int kNumFacings = 1 << kFacingShift;  // 16

inline constexpr int kSinShift = 14;
inline constexpr int kSinScale = 1 << kSinShift;  // 16384

// Transcribed from trans.c:23-89 by reading the file, not by hand. The values
// are (SIZE)(x * 16384) of the authored decimals, so they truncate toward
// zero and sit within 1 of -cos(2*pi*a/64) * 16384.
// clang-format off
inline constexpr std::array<i16, kFullCircle> kSineTab{
	-16384, -16305, -16069, -15678, -15136, -14449, -13622, -12664,
	-11585, -10393, -9102, -7723, -6269, -4756, -3196, -1605,
	0, 1605, 3196, 4756, 6269, 7723, 9102, 10393,
	11585, 12664, 13622, 14449, 15136, 15678, 16069, 16305,
	16384, 16305, 16069, 15678, 15136, 14449, 13622, 12664,
	11585, 10393, 9102, 7723, 6269, 4756, 3196, 1605,
	0, -1605, -3196, -4756, -6269, -7723, -9102, -10393,
	-11585, -12664, -13622, -14449, -15136, -15678, -16069, -16305,
};
// clang-format on

[[nodiscard]] constexpr int normalizeAngle(int a) noexcept
{
	return a & (kFullCircle - 1);
}

// ANGLE_TO_FACING rounds to the nearest facing (units.h:189); the +2 before
// the shift is the round-half-up.
[[nodiscard]] constexpr int angleToFacing(int a) noexcept
{
	return (a + (1 << (kCircleShift - kFacingShift - 1)))
			>> (kCircleShift - kFacingShift);
}

[[nodiscard]] constexpr int facingToAngle(int f) noexcept
{
	return f << (kCircleShift - kFacingShift);
}

[[nodiscard]] constexpr i16 sinVal(int a) noexcept
{
	return kSineTab[static_cast<usize>(normalizeAngle(a))];
}

[[nodiscard]] constexpr i16 cosVal(int a) noexcept
{
	return sinVal(a + kQuadrant);
}

// SINE/COSINE scale a magnitude by the table entry. The intermediate is
// widened because a magnitude times 16384 leaves the 16 bits the C's SIZE
// would have; the C widens to long for the same reason.
[[nodiscard]] constexpr i32 sine(int a, i32 m) noexcept
{
	return static_cast<i32>((static_cast<i64>(sinVal(a)) * m) >> kSinShift);
}

[[nodiscard]] constexpr i32 cosine(int a, i32 m) noexcept
{
	return sine(a + kQuadrant, m);
}

// The 33-entry quarter-turn table ARCTAN interpolates (trans.c:95-130).
// clang-format off
inline constexpr std::array<u8, 33> kArcTanTab{
	0, 0, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 6, 6, 6, 6, 7,
	7, 7, 7, 7, 7, 8, 8, 8,
};
// clang-format on

// The angle of (dx, dy), 0..63, except the zero vector returns kFullCircle
// (64) -- the C's sentinel for "no direction" (trans.c:134-135), not a
// normalized angle. Feeding it to sinVal() folds to 0; check at the call site.
[[nodiscard]] constexpr int arctan(int dx, int dy) noexcept
{
	if (dx == 0 && dy == 0)
		return kFullCircle;

	i32 v1 = dx < 0 ? -dx : dx;
	const i32 v2 = dy < 0 ? -dy : dy;

	// (32 * lesser + greater/2) / greater -- a rounded 0..32 index.
	if (v1 > v2)
	{
		const i32 i = ((v2 << (kCircleShift - 1)) + (v1 >> 1)) / v1;
		v1 = kQuadrant - kArcTanTab[static_cast<usize>(i)];
	}
	else
	{
		const i32 i = ((v1 << (kCircleShift - 1)) + (v2 >> 1)) / v2;
		v1 = kArcTanTab[static_cast<usize>(i)];
	}

	if (dx < 0)
		v1 = kFullCircle - v1;
	if (dy > 0)
		v1 = kHalfCircle - v1;

	return normalizeAngle(v1);
}

// --------------------------------------------------------------------------
// Directional integers as types (docs/cpp-conventions.md rule 7).

// A facing (0..15) and an angle (0..63) convert through a shift; both wrap
// in their constructors so arithmetic can't escape the circle. Velocity's
// travelAngle stays a plain int: wrapping would fold away ARCTAN's sentinel.

class Facing;

class Angle
{
public:
	constexpr Angle() = default;
	explicit constexpr Angle(int a) noexcept
		: v_(static_cast<u8>(a & (kFullCircle - 1)))
	{}

	[[nodiscard]] constexpr int raw() const noexcept { return v_; }
	[[nodiscard]] constexpr Facing facing() const noexcept;  // ANGLE_TO_FACING
	[[nodiscard]] constexpr Angle opposite() const noexcept
	{
		return Angle(v_ + kHalfCircle);
	}

	constexpr Angle &operator+=(int delta) noexcept
	{
		return *this = Angle(v_ + delta);
	}
	[[nodiscard]] friend constexpr Angle operator+(Angle a, int delta) noexcept
	{
		return Angle(a.v_ + delta);
	}
	[[nodiscard]] friend constexpr Angle operator-(Angle a, int delta) noexcept
	{
		return Angle(a.v_ - delta);
	}
	// The wrapped difference, 0..63 -- NORMALIZE_ANGLE(a - b).
	[[nodiscard]] friend constexpr int operator-(Angle a, Angle b) noexcept
	{
		return (a.v_ - b.v_) & (kFullCircle - 1);
	}
	friend constexpr bool operator==(Angle, Angle) = default;

private:
	u8 v_ = 0;
};

class Facing
{
public:
	constexpr Facing() = default;
	explicit constexpr Facing(int f) noexcept
		: v_(static_cast<u8>(f & (kNumFacings - 1)))
	{}

	[[nodiscard]] constexpr int raw() const noexcept { return v_; }
	[[nodiscard]] constexpr Angle angle() const noexcept  // FACING_TO_ANGLE
	{
		return Angle(v_ << (kCircleShift - kFacingShift));
	}
	[[nodiscard]] constexpr Facing opposite() const noexcept
	{
		return Facing(v_ + kNumFacings / 2);
	}

	constexpr Facing &operator+=(int delta) noexcept
	{
		return *this = Facing(v_ + delta);
	}
	constexpr Facing &operator-=(int delta) noexcept
	{
		return *this = Facing(v_ - delta);
	}
	[[nodiscard]] friend constexpr Facing operator+(
			Facing f, int delta) noexcept
	{
		return Facing(f.v_ + delta);
	}
	[[nodiscard]] friend constexpr Facing operator-(
			Facing f, int delta) noexcept
	{
		return Facing(f.v_ - delta);
	}
	// The wrapped difference, 0..15 -- NORMALIZE_FACING(a - b).
	[[nodiscard]] friend constexpr int operator-(Facing a, Facing b) noexcept
	{
		return (a.v_ - b.v_) & (kNumFacings - 1);
	}
	friend constexpr bool operator==(Facing, Facing) = default;

private:
	u8 v_ = 0;
};

constexpr Facing Angle::facing() const noexcept
{
	return Facing(angleToFacing(v_));
}

// Trig over the types; the int overloads above remain the sentinel-tolerant
// primitive layer.
[[nodiscard]] constexpr i32 sine(Angle a, i32 m) noexcept
{
	return sine(a.raw(), m);
}
[[nodiscard]] constexpr i32 cosine(Angle a, i32 m) noexcept
{
	return cosine(a.raw(), m);
}

static_assert(Facing(17).raw() == 1 && Facing(-1).raw() == 15);
static_assert(
		Angle(64).raw() == 0, "the sentinel folds to 0, as C indexing does");
static_assert(Facing(3).angle().raw() == 12);
static_assert(Angle(14).facing().raw() == 4, "rounds half up, ANGLE_TO_FACING");
static_assert(Facing(2) - Facing(15) == 3 && Facing(15) - Facing(2) == 13);
static_assert(Facing(0).opposite().raw() == 8);
static_assert(cosine(Facing(4).angle(), 1000) == 1000, "facing 4 points +x");

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
static_assert(Facing(angleToFacing(kFullCircle)).raw() == 0,
		"a facing beyond the circle wraps, the way NORMALIZE_FACING did");

}  // namespace uqm::sim

#endif  // UQM2_SIM_TRIG_HPP
