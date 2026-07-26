// Copyright the Ur-Quan Masters contributors. GPL-2.0-or-later.

#ifndef UQM2_ENGINE_CONTENT_PNGIMAGE_HPP
#define UQM2_ENGINE_CONTENT_PNGIMAGE_HPP

#include "Bytes.hpp"
#include "ColorTable.hpp"
#include "ContentError.hpp"
#include "engine/core/Geometry.hpp"

#include <cstdint>
#include <expected>
#include <span>
#include <vector>

namespace uqm::content {

enum class PixelFormat : std::uint8_t
{
	// One byte per pixel, meaningful only against a palette. Which palette is
	// the interesting part: the PNG's own PLTE is a preview, but what the
	// game draws with is the colormap slot named by the .ani. Keeping art
	// indexed is the whole reason this is not just "decode everything to
	// RGBA".
	Indexed8,

	// Straight RGBA, for images that cannot stay indexed: greyscale, true
	// colour, or a palette whose tRNS is more than a single colour key.
	Rgba8,
};

// Move-only. It holds up to megabytes, and a silent copy is a frame-time
// cliff nobody finds by reading (docs/cpp-conventions.md rule 6). Returning
// one by value moves; `auto img = other` is a compile error, which is the
// point.
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
	[[nodiscard]] std::span<const std::uint8_t> pixels() const noexcept
	{
		return pixels_;
	}
	[[nodiscard]] std::size_t bytesPerPixel() const noexcept
	{
		return format_ == PixelFormat::Indexed8 ? 1u : 4u;
	}

	// Indexed8 only. The palette is a fixed array, not a vector: a PLTE holds
	// at most 256 entries, so its size is part of the format.
	[[nodiscard]] const Palette &palette() const noexcept { return palette_; }
	[[nodiscard]] std::size_t paletteSize() const noexcept
	{
		return paletteSize_;
	}
	// -1 when the image has no transparent index.
	[[nodiscard]] std::int32_t transparentIndex() const noexcept
	{
		return transparentIndex_;
	}

	// Index at a pixel. Asserts on format and bounds: both are the caller's
	// invariant, not a content error.
	[[nodiscard]] std::uint8_t indexAt(Vec2u at) const noexcept
	{
		assert(format_ == PixelFormat::Indexed8);
		assert(size_.contains(at) && "pixel out of range");
		return pixels_[static_cast<std::size_t>(at.y) * size_.w + at.x];
	}

	// What the file said, before any of the above was decided. Kept because
	// "which of these did the content actually use" is a question the browser
	// exists to answer.
	[[nodiscard]] std::uint8_t sourceColorType() const noexcept
	{
		return sourceColorType_;
	}
	[[nodiscard]] std::uint8_t sourceBitDepth() const noexcept
	{
		return sourceBitDepth_;
	}

	friend std::expected<PngImage, ContentError> decodePng(Bytes);

private:
	std::vector<std::uint8_t> pixels_;
	Palette palette_{};
	Extent2u size_;
	std::uint16_t paletteSize_ = 0;
	std::int32_t transparentIndex_ = -1;
	PixelFormat format_ = PixelFormat::Rgba8;
	std::uint8_t sourceColorType_ = 0;
	std::uint8_t sourceBitDepth_ = 0;
};

// Decodes a whole PNG from memory.
//
// The classification follows png2sdl.c:213-300, which is the C's own careful
// mapping of libpng's transform calls onto spng's format flags. Reproduced
// rather than reinvented: getting "when does indexed art stop being indexed"
// wrong would silently change how every sprite is coloured.
[[nodiscard]] std::expected<PngImage, ContentError> decodePng(Bytes bytes);

// Encodes 8-bit RGBA. Used by the browser to write contact sheets; the game
// never writes PNGs.
[[nodiscard]] std::expected<std::vector<std::byte>, ContentError> encodeRgbaPng(
		Extent2u size, std::span<const std::uint8_t> rgba);

}  // namespace uqm::content

#endif  // UQM2_ENGINE_CONTENT_PNGIMAGE_HPP
