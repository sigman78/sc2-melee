// Copyright the Ur-Quan Masters contributors. GPL-2.0-or-later.

#include "Sprite.hpp"

namespace uqm::content {

std::vector<std::uint8_t>
toRgba(const PngImage &img, const Palette *palette)
{
	const Extent2u size = img.size();
	const std::span<const std::uint8_t> px = img.pixels();

	std::vector<std::uint8_t> out(
			static_cast<std::size_t>(size.w) * size.h * 4, 0);

	if (img.format() == PixelFormat::Rgba8)
	{
		// Already in the right shape; the copy is so callers get an owned
		// buffer with the same lifetime rules either way.
		out.assign(px.begin(), px.end());
		return out;
	}

	const std::int32_t clear = img.transparentIndex();
	for (std::uint32_t y = 0; y < size.h; ++y)
	{
		for (std::uint32_t x = 0; x < size.w; ++x)
		{
			const std::uint8_t idx = img.indexAt({x, y});
			const std::size_t at =
					(static_cast<std::size_t>(y) * size.w + x) * 4;

			if (clear >= 0 && idx == clear)
				continue;  // already zeroed, so fully transparent

			Rgb col{255, 0, 255};  // an index with no colour anywhere
			if (palette != nullptr)
				col = (*palette)[idx];
			else if (idx < img.paletteSize())
				col = img.palette()[idx];

			out[at + 0] = col.r;
			out[at + 1] = col.g;
			out[at + 2] = col.b;
			out[at + 3] = 255;
		}
	}
	return out;
}

std::vector<std::uint8_t>
opacityBits(std::span<const std::uint8_t> rgba, Extent2u size)
{
	const std::size_t count = static_cast<std::size_t>(size.w) * size.h;
	std::vector<std::uint8_t> bits(count, 0);
	for (std::size_t i = 0; i < count && (i * 4 + 3) < rgba.size(); ++i)
		bits[i] = rgba[i * 4 + 3] != 0 ? 1 : 0;
	return bits;
}

}  // namespace uqm::content
