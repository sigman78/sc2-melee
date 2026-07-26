// Copyright the Ur-Quan Masters contributors. GPL-2.0-or-later.

#ifndef UQM2_ENGINE_CONTENT_COLORTABLE_HPP
#define UQM2_ENGINE_CONTENT_COLORTABLE_HPP

#include "Bytes.hpp"

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace uqm::content {

struct Rgb
{
	std::uint8_t r = 0, g = 0, b = 0;

	friend bool operator==(const Rgb &, const Rgb &) = default;
};

inline constexpr std::size_t kPaletteSize = 256;   // NUMBER_OF_PLUTVALS
using Palette = std::array<Rgb, kPaletteSize>;

// A .ct entry, in one of the two shapes docs/content-formats.md describes.
//
// The shape is NOT recoverable from the bytes. Both start with two bytes that
// look like a range and neither carries a tag, so the same entry parses one
// way as a run of full palettes and another as a partial one -- Supox's entry
// says "10..10", which is 768 bytes of palette under one reading and 3 bytes
// under the other. The caller has to say which it expects, and it knows
// because it knows which resource key it asked for.
enum class ColorTableShape
{
	// [startSlot, endSlot] + (endSlot - startSlot + 1) full 256-entry
	// palettes. What SetColorMap (cmap.c:266-335) consumes: each palette goes
	// into a global colormap slot. 79 entries in the tree.
	Palettes,

	// [firstIndex, lastIndex] + one RGB triple per palette *index* in that
	// range. A partial palette, not a run of them. 123 entries in the tree,
	// every one a planets/*.ct covering 128..255.
	PartialPalette,
};

struct ColorTableEntry
{
	// Meaning depends on the shape: colormap slots for Palettes, palette
	// indices for PartialPalette.
	std::uint8_t first = 0;
	std::uint8_t last = 0;

	// Palettes shape: one entry per slot in [first, last].
	std::vector<Palette> palettes;

	// PartialPalette shape: colours for indices [first, last], in order.
	std::vector<Rgb> colors;
};

// Returns nullopt and sets `error` when `bytes` is not that shape. Passing
// the wrong shape reliably fails rather than silently mis-reading: the length
// check is exact, and the two shapes' lengths agree only for a degenerate
// entry that does not occur.
std::optional<ColorTableEntry> parseColorTableEntry(
		Bytes bytes, ColorTableShape shape, std::string &error);

}  // namespace uqm::content

#endif  // UQM2_ENGINE_CONTENT_COLORTABLE_HPP
