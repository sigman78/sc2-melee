// Copyright the Ur-Quan Masters contributors. GPL-2.0-or-later.

#ifndef UQM2_ENGINE_CONTENT_BYTES_HPP
#define UQM2_ENGINE_CONTENT_BYTES_HPP

#include <bit>
#include <cassert>
#include <concepts>
#include <cstddef>
#include <cstdint>
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
readBE(Bytes b, std::size_t at) noexcept
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
[[nodiscard]] constexpr std::uint8_t
readU8(Bytes b, std::size_t at) noexcept
{
	assert(at < b.size() && "readU8 past the end of the buffer");
	return std::to_integer<std::uint8_t>(b[at]);
}

[[nodiscard]] inline std::uint16_t
readU16BE(Bytes b, std::size_t at) noexcept
{
	return readBE<std::uint16_t>(b, at);
}

[[nodiscard]] inline std::uint32_t
readU32BE(Bytes b, std::size_t at) noexcept
{
	return readBE<std::uint32_t>(b, at);
}

// Does a range of `len` bytes starting at `at` fit? Written to be immune to
// the overflow that the obvious `at + len <= size` invites, since both
// operands come from file data.
[[nodiscard]] constexpr bool
fits(Bytes b, std::size_t at, std::size_t len) noexcept
{
	return at <= b.size() && len <= b.size() - at;
}

}  // namespace uqm::content

#endif  // UQM2_ENGINE_CONTENT_BYTES_HPP
