// Copyright the Ur-Quan Masters contributors. GPL-2.0-or-later.

#ifndef UQM2_ENGINE_CORE_GEOMETRY_HPP
#define UQM2_ENGINE_CORE_GEOMETRY_HPP

#include "engine/core/Types.hpp"

#include <type_traits>

namespace uqm {

// Small compound types for data that is logically one unit
// (docs/cpp-conventions.md rule 7). Loose parallel `x, y` fields invite
// transposed arguments that compile fine and draw in the wrong place.
//
// All of these are constexpr, trivially copyable, no virtuals, no
// allocation -- they pass in registers and are usable in `sim/`, which is why
// they live here rather than in a graphics header.

template <class T>
struct Vec2
{
	T x{};
	T y{};

	constexpr Vec2() = default;
	constexpr Vec2(T x_, T y_) noexcept : x(x_), y(y_) {}

	friend constexpr bool operator==(const Vec2 &, const Vec2 &) = default;

	constexpr Vec2 operator+(const Vec2 &o) const noexcept
	{
		return {static_cast<T>(x + o.x), static_cast<T>(y + o.y)};
	}
	constexpr Vec2 operator-(const Vec2 &o) const noexcept
	{
		return {static_cast<T>(x - o.x), static_cast<T>(y - o.y)};
	}
	constexpr Vec2 &operator+=(const Vec2 &o) noexcept
	{
		x = static_cast<T>(x + o.x);
		y = static_cast<T>(y + o.y);
		return *this;
	}

	template <class U>
	constexpr Vec2<U> as() const noexcept
	{
		return Vec2<U>{static_cast<U>(x), static_cast<U>(y)};
	}
};

using Vec2i = Vec2<i32>;
using Vec2u = Vec2<u32>;
using Vec2f = Vec2<float>;

// A size, deliberately not a Vec2: adding two sizes is meaningless and
// `size.x` reads worse than `size.w`. The type separation is the point.
template <class T>
struct Extent2
{
	T w{};
	T h{};

	constexpr Extent2() = default;
	constexpr Extent2(T w_, T h_) noexcept : w(w_), h(h_) {}

	friend constexpr bool operator==(const Extent2 &, const Extent2 &) = default;

	[[nodiscard]] constexpr bool empty() const noexcept
	{
		return w == T{} || h == T{};
	}

	// Widened so a 4096x4096 image does not overflow a 32-bit product.
	[[nodiscard]] constexpr u64 area() const noexcept
	{
		return static_cast<u64>(w) * static_cast<u64>(h);
	}

	[[nodiscard]] constexpr bool contains(Vec2<T> p) const noexcept
	{
		if constexpr (std::is_signed_v<T>)
		{
			if (p.x < T{} || p.y < T{})
				return false;
		}
		return p.x < w && p.y < h;
	}
};

using Extent2u = Extent2<u32>;
using Extent2i = Extent2<i32>;

// A CLOSED interval [first, last], not half-open -- see
// docs/cpp-conventions.md rule 7 for why (a uint8_t range over real content
// could not represent 128..255 as half-open). No begin()/end() on purpose.
template <class T>
struct ClosedRange
{
	T first{};
	T last{};

	constexpr ClosedRange() = default;
	constexpr ClosedRange(T first_, T last_) noexcept
		: first(first_), last(last_)
	{
	}

	friend constexpr bool operator==(
			const ClosedRange &, const ClosedRange &) = default;

	// An empty interval is not representable, so "is this well formed" is the
	// question worth asking, and callers do ask it.
	[[nodiscard]] constexpr bool valid() const noexcept { return first <= last; }

	[[nodiscard]] constexpr usize count() const noexcept
	{
		return valid() ? static_cast<usize>(last - first) + 1u : 0u;
	}

	[[nodiscard]] constexpr bool contains(T v) const noexcept
	{
		return v >= first && v <= last;
	}
};

using ClosedRangeU8 = ClosedRange<u8>;

}  // namespace uqm

#endif  // UQM2_ENGINE_CORE_GEOMETRY_HPP
