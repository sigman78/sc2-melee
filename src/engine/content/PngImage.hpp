// Copyright the Ur-Quan Masters contributors. GPL-2.0-or-later.

#ifndef UQM2_ENGINE_CONTENT_PNGIMAGE_HPP
#define UQM2_ENGINE_CONTENT_PNGIMAGE_HPP

#include "Bytes.hpp"
#include "ColorTable.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace uqm::content {

enum class PixelFormat
{
	// One byte per pixel, meaningful only against a palette. Which palette is
	// the interesting part: the PNG's own PLTE is a preview, but what the
	// game actually draws with is the colormap slot named by the .ani
	// (ColorTable.hpp). Keeping art indexed is the whole reason this is not
	// just "decode everything to RGBA".
	Indexed8,

	// Straight RGBA, for images that cannot stay indexed: greyscale, true
	// colour, or a palette whose tRNS is more than a single colour key.
	Rgba8,
};

struct PngImage
{
	PixelFormat format = PixelFormat::Rgba8;
	std::uint32_t width = 0;
	std::uint32_t height = 0;

	// Indexed8: one byte per pixel. Rgba8: four, R,G,B,A.
	std::vector<std::uint8_t> pixels;

	// Indexed8 only: the file's own PLTE, and the single transparent index
	// if it has one (-1 otherwise).
	std::vector<Rgb> palette;
	int transparentIndex = -1;

	// What the file said, before any of the above was decided. Kept because
	// "which of these did the content actually use" is a question the browser
	// exists to answer.
	unsigned sourceColorType = 0;
	unsigned sourceBitDepth = 0;

	[[nodiscard]] std::size_t bytesPerPixel () const
	{
		return format == PixelFormat::Indexed8 ? 1u : 4u;
	}
};

// Decodes a whole PNG from memory. Returns nullopt with a message rather than
// throwing, like the rest of this library.
//
// The classification follows png2sdl.c:213-300, which is the C's own careful
// mapping of libpng's transform calls onto spng's format flags. Reproduced
// rather than reinvented: getting "when does indexed art stop being indexed"
// wrong would silently change how every sprite is coloured.
std::optional<PngImage> decodePng (Bytes bytes, std::string &error);

// Encodes 8-bit RGBA. Used by the browser to write contact sheets; the game
// never writes PNGs.
std::optional<std::vector<std::byte>> encodeRgbaPng (std::uint32_t width,
		std::uint32_t height, const std::vector<std::uint8_t> &rgba,
		std::string &error);

}  // namespace uqm::content

#endif  // UQM2_ENGINE_CONTENT_PNGIMAGE_HPP
