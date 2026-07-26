// Copyright the Ur-Quan Masters contributors. GPL-2.0-or-later.

#ifndef UQM2_ENGINE_CORE_GEOMETRY_HPP
#define UQM2_ENGINE_CORE_GEOMETRY_HPP

#include <cstdint>
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

using Vec2i = Vec2<std::int32_t>;
using Vec2u = Vec2<std::uint32_t>;
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
	[[nodiscard]] constexpr std::uint64_t area() const noexcept
	{
		return static_cast<std::uint64_t>(w) * static_cast<std::uint64_t>(h);
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

using Extent2u = Extent2<std::uint32_t>;
using Extent2i = Extent2<std::int32_t>;

// A CLOSED interval [first, last] -- `last` is the last valid element, not
// one past it.
//
// That is the opposite of every range in the standard library, so it is not
// called `Range`: a bare `Range{128, 255}` would read as half-open to any C++
// programmer and be wrong by one everywhere. The name is the warning.
//
// Closed is not a preference, it is what the content is. A .ct entry says
// "slots 10 through 10" meaning one slot, and every planets/*.ct says
// "indices 128 through 255" meaning 128 colours; SetColorMap (cmap.c:308)
// loops `start <= end`. Storing that half-open would need `end = 256`, which
// does not fit in the uint8_t the format uses -- so a half-open version of
// this type could not represent the shipped content at all.
//
// Deliberately has no begin()/end(): it must not work in a range-for, where
// the half-open assumption would be silent.
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

	[[nodiscard]] constexpr std::size_t count() const noexcept
	{
		return valid() ? static_cast<std::size_t>(last - first) + 1u : 0u;
	}

	[[nodiscard]] constexpr bool contains(T v) const noexcept
	{
		return v >= first && v <= last;
	}
};

using ClosedRangeU8 = ClosedRange<std::uint8_t>;

}  // namespace uqm

#endif  // UQM2_ENGINE_CORE_GEOMETRY_HPP
