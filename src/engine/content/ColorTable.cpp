// Copyright the Ur-Quan Masters contributors. GPL-2.0-or-later.

#include "ColorTable.hpp"

namespace uqm::content {

namespace {

constexpr std::size_t kRgbSize = 3;                       // PLUTVAL_BYTE_SIZE
constexpr std::size_t kPaletteBytes = kPaletteSize * kRgbSize;   // 768

Rgb
readRgb (Bytes b, std::size_t at)
{
	// Channel order is fixed by PLUTVAL_RED/GREEN/BLUE (cmap.h:30-32).
	return Rgb{readU8 (b, at + 0), readU8 (b, at + 1), readU8 (b, at + 2)};
}

}  // namespace

std::optional<ColorTableEntry>
parseColorTableEntry (Bytes bytes, ColorTableShape shape, std::string &error)
{
	if (bytes.size () < 2)
	{
		error = "entry is too short to carry a range";
		return std::nullopt;
	}

	ColorTableEntry entry;
	entry.first = readU8 (bytes, 0);
	entry.last = readU8 (bytes, 1);

	if (entry.first > entry.last)
	{
		// SetColorMap rejects this too (cmap.c:278-284), and for a partial
		// palette it would mean a negative span.
		error = "range start " + std::to_string (entry.first)
				+ " is past its end " + std::to_string (entry.last);
		return std::nullopt;
	}

	const std::size_t span =
			static_cast<std::size_t> (entry.last - entry.first) + 1;
	const Bytes payload = bytes.subspan (2);

	// Exact, not "at least". A wrong-shape read is the failure this whole
	// type exists to prevent, and slack is how it would slip through.
	const std::size_t want =
			(shape == ColorTableShape::Palettes) ? span * kPaletteBytes
												 : span * kRgbSize;
	if (payload.size () != want)
	{
		error = std::string ("entry declares ") + std::to_string (span)
				+ (shape == ColorTableShape::Palettes
						? " palette(s), needing "
						: " colour(s), needing ")
				+ std::to_string (want) + " bytes, but carries "
				+ std::to_string (payload.size ());
		return std::nullopt;
	}

	if (shape == ColorTableShape::Palettes)
	{
		entry.palettes.resize (span);
		for (std::size_t p = 0; p < span; ++p)
		{
			for (std::size_t i = 0; i < kPaletteSize; ++i)
			{
				entry.palettes[p][i] =
						readRgb (payload, p * kPaletteBytes + i * kRgbSize);
			}
		}
	}
	else
	{
		entry.colors.resize (span);
		for (std::size_t i = 0; i < span; ++i)
			entry.colors[i] = readRgb (payload, i * kRgbSize);
	}

	return entry;
}

}  // namespace uqm::content
