// Copyright the Ur-Quan Masters contributors. GPL-2.0-or-later.

#include "Sprite.hpp"

#include "engine/core/Types.hpp"

namespace uqm::content {

std::vector<u8> toRgba(const PngImage &img, const Palette *palette)
{
	const Extent2u size = img.size();
	const std::span<const u8> px = img.pixels();

	std::vector<u8> out(static_cast<usize>(size.w) * size.h * 4, 0);

	if (img.format() == PixelFormat::Rgba8)
	{
		// Already in the right shape; the copy is so callers get an owned
		// buffer with the same lifetime rules either way.
		out.assign(px.begin(), px.end());
		return out;
	}

	const i32 clear = img.transparentIndex();
	for (u32 y = 0; y < size.h; ++y)
	{
		for (u32 x = 0; x < size.w; ++x)
		{
			const u8 idx = img.indexAt({x, y});
			const usize at = (static_cast<usize>(y) * size.w + x) * 4;

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

std::vector<u8> opacityBits(std::span<const u8> rgba, Extent2u size)
{
	const usize count = static_cast<usize>(size.w) * size.h;
	std::vector<u8> bits(count, 0);
	for (usize i = 0; i < count && (i * 4 + 3) < rgba.size(); ++i)
		bits[i] = rgba[i * 4 + 3] != 0 ? 1 : 0;
	return bits;
}

}  // namespace uqm::content
