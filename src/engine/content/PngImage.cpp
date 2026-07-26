// Copyright the Ur-Quan Masters contributors. GPL-2.0-or-later.

#include "PngImage.hpp"

#include <spng.h>

#include <cstdlib>
#include <cstring>
#include <memory>

namespace uqm::content {

namespace {

struct CtxDeleter
{
	void operator() (spng_ctx *c) const { spng_ctx_free (c); }
};
using CtxPtr = std::unique_ptr<spng_ctx, CtxDeleter>;

// png2sdl.c:92-110. A tRNS on an indexed image can stay a colour key only if
// exactly one entry is fully transparent and none is partial. Anything else
// has to become real alpha, which means giving up the indices.
bool
trnsIsColorKey (const spng_trns &trns, int &key)
{
	int found = -1;
	for (std::uint32_t i = 0; i < trns.n_type3_entries; ++i)
	{
		if (trns.type3_alpha[i] == 0)
		{
			if (found >= 0)
				return false;  // a second fully-transparent index
			found = static_cast<int> (i);
		}
		else if (trns.type3_alpha[i] != 255)
			return false;  // translucency
	}
	key = found;
	return true;
}

}  // namespace

std::optional<PngImage>
decodePng (Bytes bytes, std::string &error)
{
	if (bytes.empty ())
	{
		error = "empty file";
		return std::nullopt;
	}

	CtxPtr ctx (spng_ctx_new (0));
	if (!ctx)
	{
		error = "could not allocate an spng context";
		return std::nullopt;
	}

	// Content PNGs are small and already in memory; no streaming needed.
	if (const int err = spng_set_png_buffer (
				ctx.get (), bytes.data (), bytes.size ()))
	{
		error = spng_strerror (err);
		return std::nullopt;
	}

	spng_ihdr ihdr{};
	if (const int err = spng_get_ihdr (ctx.get (), &ihdr))
	{
		error = spng_strerror (err);
		return std::nullopt;
	}

	// spng_get_ihdr has walked the chunks up to the first IDAT, so PLTE and
	// tRNS are known by now.
	spng_plte plte{};
	spng_trns trns{};
	const bool havePlte = spng_get_plte (ctx.get (), &plte) == 0;
	const bool haveTrns = spng_get_trns (ctx.get (), &trns) == 0;

	PngImage image;
	image.width = ihdr.width;
	image.height = ihdr.height;
	image.sourceColorType = ihdr.color_type;
	image.sourceBitDepth = ihdr.bit_depth;

	// Can this stay indexed? Only an indexed source with a colour-key (or
	// absent) tRNS. Everything else goes to RGBA and lets spng do the work.
	int colorKey = -1;
	const bool keepIndexed = ihdr.color_type == SPNG_COLOR_TYPE_INDEXED
			&& havePlte
			&& (!haveTrns || trnsIsColorKey (trns, colorKey));

	const int fmt = keepIndexed ? SPNG_FMT_PNG : SPNG_FMT_RGBA8;
	const int flags = keepIndexed ? 0 : SPNG_DECODE_TRNS;

	std::size_t decodedSize = 0;
	if (const int err =
					spng_decoded_image_size (ctx.get (), fmt, &decodedSize))
	{
		error = spng_strerror (err);
		return std::nullopt;
	}

	std::vector<std::uint8_t> raw (decodedSize);
	if (const int err = spng_decode_image (
				ctx.get (), raw.data (), raw.size (), fmt, flags))
	{
		error = spng_strerror (err);
		return std::nullopt;
	}

	if (!keepIndexed)
	{
		image.format = PixelFormat::Rgba8;
		image.pixels = std::move (raw);
		return image;
	}

	image.format = PixelFormat::Indexed8;
	image.transparentIndex = colorKey;
	image.palette.resize (plte.n_entries);
	for (std::uint32_t i = 0; i < plte.n_entries; ++i)
	{
		image.palette[i] = Rgb{plte.entries[i].red, plte.entries[i].green,
			plte.entries[i].blue};
	}

	// SPNG_FMT_PNG keeps the file's own packing, so 1/2/4-bpp rows arrive
	// packed and have to be widened. Widened, not rescaled: these are
	// indices, and scaling them would silently recolour the image
	// (png2sdl.c:120-132 makes the same point).
	if (ihdr.bit_depth == 8)
	{
		image.pixels = std::move (raw);
	}
	else
	{
		const std::size_t stride =
				(static_cast<std::size_t> (ihdr.width) * ihdr.bit_depth + 7) / 8;
		if (raw.size () < stride * ihdr.height)
		{
			error = "decoded buffer is smaller than the packed rows need";
			return std::nullopt;
		}

		image.pixels.assign (
				static_cast<std::size_t> (ihdr.width) * ihdr.height, 0);
		const unsigned mask = (1u << ihdr.bit_depth) - 1u;
		for (std::uint32_t y = 0; y < ihdr.height; ++y)
		{
			const std::uint8_t *src = raw.data () + stride * y;
			std::uint8_t *dst =
					image.pixels.data () + std::size_t{ihdr.width} * y;
			for (std::uint32_t x = 0; x < ihdr.width; ++x)
			{
				// PNG packs the leftmost pixel in the most significant bits.
				const unsigned bit = x * ihdr.bit_depth;
				dst[x] = static_cast<std::uint8_t> (
						(src[bit / 8] >> (8 - ihdr.bit_depth - bit % 8))
						& mask);
			}
		}
	}

	return image;
}

std::optional<std::vector<std::byte>>
encodeRgbaPng (std::uint32_t width, std::uint32_t height,
		const std::vector<std::uint8_t> &rgba, std::string &error)
{
	if (rgba.size () != std::size_t{width} * height * 4)
	{
		error = "pixel buffer is " + std::to_string (rgba.size ())
				+ " bytes, expected " + std::to_string (std::size_t{width} * height * 4);
		return std::nullopt;
	}

	CtxPtr ctx (spng_ctx_new (SPNG_CTX_ENCODER));
	if (!ctx)
	{
		error = "could not allocate an spng encoder";
		return std::nullopt;
	}

	spng_set_option (ctx.get (), SPNG_ENCODE_TO_BUFFER, 1);

	spng_ihdr ihdr{};
	ihdr.width = width;
	ihdr.height = height;
	ihdr.bit_depth = 8;
	ihdr.color_type = SPNG_COLOR_TYPE_TRUECOLOR_ALPHA;
	if (const int err = spng_set_ihdr (ctx.get (), &ihdr))
	{
		error = spng_strerror (err);
		return std::nullopt;
	}

	if (const int err = spng_encode_image (ctx.get (), rgba.data (),
				rgba.size (), SPNG_FMT_PNG, SPNG_ENCODE_FINALIZE))
	{
		error = spng_strerror (err);
		return std::nullopt;
	}

	int err = 0;
	std::size_t size = 0;
	void *buf = spng_get_png_buffer (ctx.get (), &size, &err);
	if (!buf)
	{
		error = spng_strerror (err);
		return std::nullopt;
	}

	std::vector<std::byte> out (size);
	std::memcpy (out.data (), buf, size);
	std::free (buf);  // spng_get_png_buffer hands over ownership
	return out;
}

}  // namespace uqm::content
