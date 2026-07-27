// Copyright the Ur-Quan Masters contributors. GPL-2.0-or-later.

#ifndef UQM2_ENGINE_CONTENT_COLORTABLE_HPP
#define UQM2_ENGINE_CONTENT_COLORTABLE_HPP

#include "Bytes.hpp"
#include "ContentError.hpp"
#include "engine/core/Geometry.hpp"
#include "engine/core/Types.hpp"

#include <array>
#include <expected>

namespace uqm::content {

struct Rgb
{
	u8 r = 0;
	u8 g = 0;
	u8 b = 0;

	friend constexpr bool operator==(const Rgb &, const Rgb &) = default;
};

inline constexpr usize kPaletteSize = 256;   // NUMBER_OF_PLUTVALS
inline constexpr usize kRgbSize = 3;         // PLUTVAL_BYTE_SIZE

// A palette is a fixed-size value: 256 x 3 bytes, no allocation, trivially
// copyable, and byte-for-byte the file's own layout
// (docs/cpp-conventions.md rule 1).
using Palette = std::array<Rgb, kPaletteSize>;

static_assert(sizeof(Rgb) == kRgbSize, "Rgb must be exactly three bytes");
static_assert(sizeof(Palette) == kPaletteSize * kRgbSize,
		"Palette must match the file layout so it can be copied wholesale");

inline constexpr usize kPaletteBytes = sizeof(Palette);

// A .ct entry's shape (docs/content-formats.md) is not recoverable from the
// bytes -- both open with an untagged range-like pair, so Supox's "10..10"
// is 768 bytes as a palette run but 3 as a partial one. Caller says which.
enum class ColorTableShape : u8
{
	// [startSlot, endSlot] + one full 256-entry palette per slot. What
	// SetColorMap (cmap.c:266-335) consumes. 79 entries in the tree.
	Palettes,

	// [firstIndex, lastIndex] + one RGB triple per palette *index* in that
	// range -- a partial palette, not a run of them. 123 entries, every one a
	// planets/*.ct covering 128..255.
	PartialPalette,
};

// LIFETIME: holds a view of the entry bytes, which are themselves a view into
// the file buffer. The buffer outlives it.
class ColorTableEntry
{
public:
	constexpr ColorTableEntry() = default;

	// Colormap slots for Palettes; palette indices for PartialPalette.
	[[nodiscard]] constexpr ClosedRangeU8 range() const noexcept { return range_; }
	[[nodiscard]] constexpr ColorTableShape shape() const noexcept
	{
		return shape_;
	}

	// --- Palettes shape ---

	[[nodiscard]] constexpr usize paletteCount() const noexcept
	{
		return shape_ == ColorTableShape::Palettes ? range_.count() : 0u;
	}

	// By value: 768 bytes off the stack, no allocation, and the copy is a
	// straight memcpy because the layouts are identical.
	[[nodiscard]] Palette palette(usize i) const noexcept;

	// --- PartialPalette shape ---

	[[nodiscard]] constexpr usize colorCount() const noexcept
	{
		return shape_ == ColorTableShape::PartialPalette ? range_.count() : 0u;
	}
	[[nodiscard]] constexpr Rgb color(usize i) const noexcept
	{
		assert(shape_ == ColorTableShape::PartialPalette && i < colorCount());
		return Rgb{readU8(payload_, i * kRgbSize),
			readU8(payload_, i * kRgbSize + 1),
			readU8(payload_, i * kRgbSize + 2)};
	}

	friend std::expected<ColorTableEntry, ContentError> parseColorTableEntry(
			Bytes, ColorTableShape);

private:
	ClosedRangeU8 range_;
	Bytes payload_;
	ColorTableShape shape_ = ColorTableShape::Palettes;
};

// Passing the wrong shape fails rather than silently mis-reading: the length
// check is exact, and the two shapes agree only for a degenerate entry that
// does not occur.
[[nodiscard]] std::expected<ColorTableEntry, ContentError> parseColorTableEntry(
		Bytes bytes, ColorTableShape shape);

}  // namespace uqm::content

#endif  // UQM2_ENGINE_CONTENT_COLORTABLE_HPP
