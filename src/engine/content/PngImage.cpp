// Copyright the Ur-Quan Masters contributors. GPL-2.0-or-later.

#include "PngImage.hpp"

#include "engine/core/Types.hpp"

#include <cstdlib>
#include <cstring>
#include <memory>
#include <spng.h>
#include <utility>

namespace uqm::content {

namespace {

struct CtxDeleter
{
	void operator()(spng_ctx *c) const noexcept { spng_ctx_free(c); }
};
using CtxPtr = std::unique_ptr<spng_ctx, CtxDeleter>;

// spng_get_png_buffer hands over ownership of a malloc'd block.
struct MallocDeleter
{
	void operator()(void *p) const noexcept { std::free(p); }
};

// png2sdl.c:92-110. A tRNS on an indexed image can stay a colour key only if
// exactly one entry is fully transparent and none is partial. Anything else
// has to become real alpha, which means giving up the indices.
bool
trnsIsColorKey(const spng_trns &trns, i32 &key) noexcept
{
	i32 found = -1;
	for (u32 i = 0; i < trns.n_type3_entries; ++i)
	{
		if (trns.type3_alpha[i] == 0)
		{
			if (found >= 0)
				return false;  // a second fully-transparent index
			found = static_cast<i32>(i);
		}
		else if (trns.type3_alpha[i] != 255)
			return false;  // translucency
	}
	key = found;
	return true;
}

}  // namespace

std::expected<PngImage, ContentError>
decodePng(Bytes bytes)
{
	using enum ContentErrorCode;

	if (bytes.empty())
		return std::unexpected(ContentError{Empty});

	CtxPtr ctx(spng_ctx_new(0));
	// Allocation failure is not a content error and there is nothing to
	// recover to (docs/cpp-conventions.md rule 3).
	assert(ctx && "out of memory allocating an spng context");

	// Content PNGs are small and already in memory; no streaming needed.
	if (spng_set_png_buffer(ctx.get(), bytes.data(), bytes.size()) != 0)
		return std::unexpected(ContentError{DecodeFailed});

	spng_ihdr ihdr{};
	if (spng_get_ihdr(ctx.get(), &ihdr) != 0)
		return std::unexpected(ContentError{DecodeFailed});

	// spng_get_ihdr has walked the chunks up to the first IDAT, so PLTE and
	// tRNS are known by now.
	spng_plte plte{};
	spng_trns trns{};
	const bool havePlte = spng_get_plte(ctx.get(), &plte) == 0;
	const bool haveTrns = spng_get_trns(ctx.get(), &trns) == 0;

	PngImage image;
	image.size_ = Extent2u{ihdr.width, ihdr.height};
	image.sourceColorType_ = ihdr.color_type;
	image.sourceBitDepth_ = ihdr.bit_depth;

	// Can this stay indexed? Only an indexed source with a colour-key (or
	// absent) tRNS. Everything else goes to RGBA and lets spng do the work.
	i32 colorKey = -1;
	const bool keepIndexed = ihdr.color_type == SPNG_COLOR_TYPE_INDEXED
			&& havePlte && (!haveTrns || trnsIsColorKey(trns, colorKey));

	const int fmt = keepIndexed ? SPNG_FMT_PNG : SPNG_FMT_RGBA8;
	const int flags = keepIndexed ? 0 : SPNG_DECODE_TRNS;

	usize decodedSize = 0;
	if (spng_decoded_image_size(ctx.get(), fmt, &decodedSize) != 0)
		return std::unexpected(ContentError{DecodeFailed});

	std::vector<u8> raw(decodedSize);
	if (spng_decode_image(ctx.get(), raw.data(), raw.size(), fmt, flags) != 0)
		return std::unexpected(ContentError{DecodeFailed});

	if (!keepIndexed)
	{
		image.format_ = PixelFormat::Rgba8;
		image.pixels_ = std::move(raw);
		return image;
	}

	image.format_ = PixelFormat::Indexed8;
	image.transparentIndex_ = colorKey;
	image.paletteSize_ = static_cast<u16>(plte.n_entries);
	for (u32 i = 0; i < plte.n_entries && i < kPaletteSize; ++i)
	{
		image.palette_[i] = Rgb{plte.entries[i].red, plte.entries[i].green,
			plte.entries[i].blue};
	}

	// SPNG_FMT_PNG keeps the file's packing, so sub-8-bit rows must be
	// widened, not rescaled -- these are indices, and scaling would recolor
	// the image (png2sdl.c:120-132). Three files in the tree exercise this path.
	if (ihdr.bit_depth == 8)
	{
		image.pixels_ = std::move(raw);
		return image;
	}

	const usize stride =
			(static_cast<usize>(ihdr.width) * ihdr.bit_depth + 7) / 8;
	if (raw.size() < stride * ihdr.height)
		return std::unexpected(ContentError{DecodeFailed});

	image.pixels_.assign(static_cast<usize>(ihdr.width) * ihdr.height, 0);
	const unsigned mask = (1u << ihdr.bit_depth) - 1u;
	for (u32 y = 0; y < ihdr.height; ++y)
	{
		const u8 *src = raw.data() + stride * y;
		u8 *dst = image.pixels_.data() + usize{ihdr.width} * y;
		for (u32 x = 0; x < ihdr.width; ++x)
		{
			// PNG packs the leftmost pixel in the most significant bits.
			const unsigned bit = x * ihdr.bit_depth;
			dst[x] = static_cast<u8>(
					(src[bit / 8] >> (8 - ihdr.bit_depth - bit % 8)) & mask);
		}
	}

	return image;
}

std::expected<std::vector<std::byte>, ContentError>
encodeRgbaPng(Extent2u size, std::span<const u8> rgba)
{
	using enum ContentErrorCode;

	const u64 want = size.area() * 4u;
	if (rgba.size() != want)
		return std::unexpected(ContentError{WrongSize, 0, want, rgba.size()});

	CtxPtr ctx(spng_ctx_new(SPNG_CTX_ENCODER));
	assert(ctx && "out of memory allocating an spng encoder");

	spng_set_option(ctx.get(), SPNG_ENCODE_TO_BUFFER, 1);

	spng_ihdr ihdr{};
	ihdr.width = size.w;
	ihdr.height = size.h;
	ihdr.bit_depth = 8;
	ihdr.color_type = SPNG_COLOR_TYPE_TRUECOLOR_ALPHA;
	if (spng_set_ihdr(ctx.get(), &ihdr) != 0)
		return std::unexpected(ContentError{EncodeFailed});

	if (spng_encode_image(ctx.get(), rgba.data(), rgba.size(), SPNG_FMT_PNG,
				SPNG_ENCODE_FINALIZE)
			!= 0)
		return std::unexpected(ContentError{EncodeFailed});

	int err = 0;
	usize encoded = 0;
	std::unique_ptr<void, MallocDeleter> buf(
			spng_get_png_buffer(ctx.get(), &encoded, &err));
	if (!buf)
		return std::unexpected(ContentError{EncodeFailed});

	std::vector<std::byte> out(encoded);
	std::memcpy(out.data(), buf.get(), encoded);
	return out;
}

}  // namespace uqm::content
