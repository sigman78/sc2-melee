// Copyright the Ur-Quan Masters contributors. GPL-2.0-or-later.

#ifndef UQM2_ENGINE_CONTENT_BYTES_HPP
#define UQM2_ENGINE_CONTENT_BYTES_HPP

#include "engine/core/Types.hpp"

#include <bit>
#include <cassert>
#include <concepts>
#include <cstddef>
#include <cstring>
#include <span>

namespace uqm::content {

using Bytes = std::span<const std::byte>;

// The content pack is big-endian throughout (docs/content-formats.md).
//
// One load plus std::byteswap (an intrinsic; folds away at compile time on
// big-endian), not a shift-and-or loop. Parameterised on the integer type,
// not a byte count, so there is exactly one body to unroll.
//
// Bounds are the caller's business: reading past the end is a broken
// invariant, not malformed content, so it asserts rather than returning an
// error once the caller skips `fits()` (docs/cpp-conventions.md rule 3).
template <class T>
	requires std::unsigned_integral<T>
[[nodiscard]] inline T
readBE(Bytes b, usize at) noexcept
{
	assert(at + sizeof(T) <= b.size() && "readBE past the end of the buffer");
	T v;
	std::memcpy(&v, b.data() + at, sizeof(T));
	if constexpr (sizeof(T) > 1 && std::endian::native == std::endian::little)
		return std::byteswap(v);
	else
		return v;
}

// Named for the widths the formats actually use. A single byte needs no
// swap and no memcpy, and staying constexpr lets the palette accessors in
// ColorTable.hpp stay constexpr too.
[[nodiscard]] constexpr u8
readU8(Bytes b, usize at) noexcept
{
	assert(at < b.size() && "readU8 past the end of the buffer");
	return std::to_integer<u8>(b[at]);
}

[[nodiscard]] inline u16
readU16BE(Bytes b, usize at) noexcept
{
	return readBE<u16>(b, at);
}

[[nodiscard]] inline u32
readU32BE(Bytes b, usize at) noexcept
{
	return readBE<u32>(b, at);
}

// Does a range of `len` bytes starting at `at` fit? Written to be immune to
// the overflow that the obvious `at + len <= size` invites, since both
// operands come from file data.
[[nodiscard]] constexpr bool
fits(Bytes b, usize at, usize len) noexcept
{
	return at <= b.size() && len <= b.size() - at;
}

}  // namespace uqm::content

#endif  // UQM2_ENGINE_CONTENT_BYTES_HPP
