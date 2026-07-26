// Copyright the Ur-Quan Masters contributors. GPL-2.0-or-later.

#ifndef UQM2_ENGINE_CONTENT_BYTES_HPP
#define UQM2_ENGINE_CONTENT_BYTES_HPP

#include <cstddef>
#include <cstdint>
#include <span>

namespace uqm::content {

using Bytes = std::span<const std::byte>;

// The content pack is big-endian throughout (docs/content-formats.md). These
// read from a span rather than a struct overlay on purpose: nothing in the
// pack is aligned, and several headers are followed immediately by unaligned
// payload.

inline std::uint8_t
readU8(Bytes b, std::size_t at)
{
	return static_cast<std::uint8_t>(b[at]);
}

inline std::uint32_t
readU32BE(Bytes b, std::size_t at)
{
	return (static_cast<std::uint32_t>(readU8(b, at + 0)) << 24)
			| (static_cast<std::uint32_t>(readU8(b, at + 1)) << 16)
			| (static_cast<std::uint32_t>(readU8(b, at + 2)) << 8)
			| static_cast<std::uint32_t>(readU8(b, at + 3));
}

// Bounds-checked subspan. Returns an empty span when the range does not fit,
// which every caller here treats as malformed rather than as an empty result.
inline bool
fits(Bytes b, std::size_t at, std::size_t len)
{
	return at <= b.size() && len <= b.size() - at;
}

}  // namespace uqm::content

#endif  // UQM2_ENGINE_CONTENT_BYTES_HPP
