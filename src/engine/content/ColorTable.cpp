// Copyright the Ur-Quan Masters contributors. GPL-2.0-or-later.

#include "ColorTable.hpp"

#include "engine/core/Types.hpp"

#include <cstring>

namespace uqm::content {

Palette ColorTableEntry::palette(usize i) const noexcept
{
	assert(shape_ == ColorTableShape::Palettes && i < paletteCount());
	Palette out;
	// The static_asserts in the header are what make this a copy rather than
	// a decode: Rgb is three bytes, the array is 768, and the file's channel
	// order is already PLUTVAL_RED/GREEN/BLUE (cmap.h:30-32).
	std::memcpy(out.data(), payload_.data() + i * kPaletteBytes, kPaletteBytes);
	return out;
}

std::expected<ColorTableEntry, ContentError> parseColorTableEntry(
		Bytes bytes, ColorTableShape shape)
{
	using enum ContentErrorCode;

	if (bytes.size() < 2)
		return std::unexpected(ContentError{TooShort, 0, 2, bytes.size()});

	const ClosedRangeU8 range{readU8(bytes, 0), readU8(bytes, 1)};
	if (!range.valid())
	{
		// SetColorMap rejects this too (cmap.c:278-284), and for a partial
		// palette it would mean a negative span.
		return std::unexpected(
				ContentError{InvertedRange, 0, range.first, range.last});
	}

	const Bytes payload = bytes.subspan(2);
	const usize stride =
			shape == ColorTableShape::Palettes ? kPaletteBytes : kRgbSize;
	const usize want = range.count() * stride;

	// Exact, not "at least". A wrong-shape read is the failure this type
	// exists to prevent, and slack is how it would slip through.
	if (payload.size() != want)
		return std::unexpected(
				ContentError{WrongSize, 0, want, payload.size()});

	ColorTableEntry entry;
	entry.range_ = range;
	entry.payload_ = payload;
	entry.shape_ = shape;
	return entry;
}

}  // namespace uqm::content
