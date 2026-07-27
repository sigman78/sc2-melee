// Copyright the Ur-Quan Masters contributors. GPL-2.0-or-later.

#ifndef UQM2_ENGINE_CONTENT_SPRITE_HPP
#define UQM2_ENGINE_CONTENT_SPRITE_HPP

#include "ColorTable.hpp"
#include "PngImage.hpp"
#include "engine/core/Geometry.hpp"

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace uqm::content {

// Turning decoded content into something both the renderer and the simulation
// can use.
//
// The browser had this logic first and kept it to itself, which was fine while
// it was the only consumer. It is not any more: the game needs exactly the
// same expansion, and a second copy would be a second place for the
// colormap-versus-PLTE distinction to drift. That distinction is not cosmetic
// -- docs/content-formats.md records that a `.ct` and a PNG `PLTE` disagree by
// construction, `(v << 2) | (v >> 4)` against `v << 2`, so a sprite coloured
// through the wrong one is visibly wrong rather than subtly so.

// Expands a decoded PNG to 8-bit RGBA.
//
// Indexed images are coloured through `palette` when one is supplied -- the
// colormap slot named by the cel -- and fall back to the PNG's own PLTE when
// it is not. An index with no colour anywhere comes out magenta rather than
// silently black, because a missing colormap should look like a bug.
[[nodiscard]] std::vector<std::uint8_t> toRgba(
		const PngImage &img, const Palette *palette = nullptr);

// One byte per pixel, 1 where the pixel is not fully transparent.
//
// This is what a collision mask is built from, and building it from the sprite
// rather than from a bounding box is the whole point of per-pixel collision --
// intersec.c tests the actual silhouettes, so a Cruiser's nose misses where its
// bounding box would hit. Returned as bytes rather than as a CollisionMask
// because that type belongs to sim/, and content does not depend on sim.
[[nodiscard]] std::vector<std::uint8_t> opacityBits(
		std::span<const std::uint8_t> rgba, Extent2u size);

}  // namespace uqm::content

#endif  // UQM2_ENGINE_CONTENT_SPRITE_HPP
