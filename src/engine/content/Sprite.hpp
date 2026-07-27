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

// Turning decoded content into something both the renderer and the
// simulation can use, so the colormap-versus-PLTE distinction has one
// implementation. docs/content-formats.md: a `.ct` and a PNG `PLTE` disagree
// by construction, `(v << 2) | (v >> 4)` against `v << 2` -- the wrong one
// colours a sprite visibly wrong.

// Expands a decoded PNG to 8-bit RGBA. Coloured through `palette` when
// given, else the PNG's own PLTE; an index with no colour anywhere comes
// out magenta, not black, so a missing colormap looks like a bug.
[[nodiscard]] std::vector<std::uint8_t> toRgba(
		const PngImage &img, const Palette *palette = nullptr);

// One byte per pixel, 1 where not fully transparent. Built from the sprite,
// not a bounding box, per intersec.c's actual-silhouette test (see
// design-notes.md#D2). Bytes, not CollisionMask: that type belongs to sim/.
[[nodiscard]] std::vector<std::uint8_t> opacityBits(
		std::span<const std::uint8_t> rgba, Extent2u size);

}  // namespace uqm::content

#endif  // UQM2_ENGINE_CONTENT_SPRITE_HPP
