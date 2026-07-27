// Copyright the Ur-Quan Masters contributors. GPL-2.0-or-later.

#ifndef UQM2_GAME_SPRITESET_HPP
#define UQM2_GAME_SPRITESET_HPP

#include "platform/Platform.hpp"
#include "sim/Collision.hpp"

#include <cstddef>
#include <filesystem>
#include <vector>

namespace uqm::game {

// A set of cels -- a ship's sixteen facings, a projectile's frames, a rock --
// uploaded, with collision masks to match.
//
// Only `-big` is loaded: the C's three pre-rendered sizes, picked by camera
// reduction under the old sprite-LOD fork, collapse to one continuous zoom
// (Camera.hpp, design-notes.md D6); smaller cels stay in the content unused.
//
// Collision masks come from `-big` and do not change with zoom
// (design-notes.md V1).
struct SpriteSet
{
	std::vector<platform::Texture> frames;
	std::vector<sim::CollisionMask> masks;

	// Every opaque pixel forced white, so colour-mod tints to a flat fill --
	// STAMPFILL_PRIM, how the C draws a cloaking Ilwrath (ilwrath.c:250-285).
	// Tinting the real cels instead only darkens them; colour-mod multiplies.
	std::vector<platform::Texture> silhouettes;

	[[nodiscard]] bool
	valid() const noexcept
	{
		return !frames.empty() && frames.size() == masks.size();
	}

	// Wraps, because facings run 0..15 and callers do arithmetic on them.
	[[nodiscard]] const sim::CollisionMask *
	maskFor(int facing) const noexcept
	{
		if (masks.empty())
			return nullptr;
		return &masks[static_cast<std::size_t>(facing)
				% masks.size()];
	}
};

// Loads `ani` and everything it names, relative to the .ani's directory;
// empty on failure (content problems are reported louder by the browser and
// CI). `colortable` is the global palette table; cels name a slot in it.
[[nodiscard]] SpriteSet loadSprites(platform::Platform &window,
		const std::filesystem::path &ani,
		const std::filesystem::path &colortable);

}  // namespace uqm::game

#endif  // UQM2_GAME_SPRITESET_HPP
