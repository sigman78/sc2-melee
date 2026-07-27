// Copyright the Ur-Quan Masters contributors. GPL-2.0-or-later.

#ifndef UQM2_ENGINE_CONTENT_PNGIMAGE_HPP
#define UQM2_ENGINE_CONTENT_PNGIMAGE_HPP

#include "Bytes.hpp"
#include "ColorTable.hpp"
#include "ContentError.hpp"
#include "engine/core/Geometry.hpp"
#include "engine/core/Types.hpp"

#include <expected>
#include <span>
#include <vector>

namespace uqm::content {

enum class PixelFormat : u8
{
	// One byte per pixel, valid only against a palette -- the PNG's own PLTE
	// is just a preview; the game colours with the .ani's colormap slot
	// instead. Keeping art indexed instead of always decoding to RGBA is the point.
	Indexed8,

	// Straight RGBA, for images that cannot stay indexed: greyscale, true
	// colour, or a palette whose tRNS is more than a single colour key.
	Rgba8,
};

// Move-only: a silent multi-megabyte copy is a frame-time cliff nobody
// finds by reading (docs/cpp-conventions.md rule 6). Returning by value
// moves; `auto img = other` is a compile error, which is the point.
class PngImage
{
public:
	PngImage() = default;

	PngImage(const PngImage &) = delete;
	PngImage &operator=(const PngImage &) = delete;
	PngImage(PngImage &&) noexcept = default;
	PngImage &operator=(PngImage &&) noexcept = default;

	[[nodiscard]] PixelFormat format() const noexcept { return format_; }
	[[nodiscard]] Extent2u size() const noexcept { return size_; }

	// Indexed8: one byte per pixel. Rgba8: four, R,G,B,A.
	[[nodiscard]] std::span<const u8> pixels() const noexcept
	{
		return pixels_;
	}
	[[nodiscard]] usize bytesPerPixel() const noexcept
	{
		return format_ == PixelFormat::Indexed8 ? 1u : 4u;
	}

	// Indexed8 only. The palette is a fixed array, not a vector: a PLTE holds
	// at most 256 entries, so its size is part of the format.
	[[nodiscard]] const Palette &palette() const noexcept { return palette_; }
	[[nodiscard]] usize paletteSize() const noexcept
	{
		return paletteSize_;
	}
	// -1 when the image has no transparent index.
	[[nodiscard]] i32 transparentIndex() const noexcept
	{
		return transparentIndex_;
	}

	// Index at a pixel. Asserts on format and bounds: both are the caller's
	// invariant, not a content error.
	[[nodiscard]] u8 indexAt(Vec2u at) const noexcept
	{
		assert(format_ == PixelFormat::Indexed8);
		assert(size_.contains(at) && "pixel out of range");
		return pixels_[static_cast<usize>(at.y) * size_.w + at.x];
	}

	// What the file said, before any of the above was decided. Kept because
	// "which of these did the content actually use" is a question the browser
	// exists to answer.
	[[nodiscard]] u8 sourceColorType() const noexcept
	{
		return sourceColorType_;
	}
	[[nodiscard]] u8 sourceBitDepth() const noexcept
	{
		return sourceBitDepth_;
	}

	friend std::expected<PngImage, ContentError> decodePng(Bytes);

private:
	std::vector<u8> pixels_;
	Palette palette_{};
	Extent2u size_;
	u16 paletteSize_ = 0;
	i32 transparentIndex_ = -1;
	PixelFormat format_ = PixelFormat::Rgba8;
	u8 sourceColorType_ = 0;
	u8 sourceBitDepth_ = 0;
};

// Decodes a whole PNG. Classification follows png2sdl.c:213-300's mapping
// of libpng transforms onto spng flags, reproduced rather than reinvented --
// wrong here silently recolors every sprite.
[[nodiscard]] std::expected<PngImage, ContentError> decodePng(Bytes bytes);

// Encodes 8-bit RGBA. Used by the browser to write contact sheets; the game
// never writes PNGs.
[[nodiscard]] std::expected<std::vector<std::byte>, ContentError> encodeRgbaPng(
		Extent2u size, std::span<const u8> rgba);

}  // namespace uqm::content

#endif  // UQM2_ENGINE_CONTENT_PNGIMAGE_HPP
