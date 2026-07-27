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
// The C ships three pre-rendered sizes per object -- cruiser-big, -med, -sml
// -- and picks one from the camera's reduction, which is the sprite-LOD half
// of the optMeleeScale fork. Only `-big` is loaded here. The camera is a
// single continuous zoom now (see Camera.hpp), so there is no reduction level
// to index with, and scaling one sprite with nearest-neighbour is what a
// continuous zoom implies. The smaller cels stay in the content and are
// available if the stepped camera ever wants them back.
//
// **Collision masks come from `-big` and do not change with zoom.** In the C
// they do: intersec.c tests whatever frame is currently displayed, so hitboxes
// shrink as the view pulls out and two ships that would touch at 1:1 pass
// through each other at 4:1. That is a presentation detail reaching into the
// simulation, which the plan forbids -- so the silhouette is fixed at the 1:1
// one. This is a deliberate divergence and it changes collisions at range.
struct SpriteSet
{
	std::vector<platform::Texture> frames;
	std::vector<sim::CollisionMask> masks;

	// The same cels with every opaque pixel forced white, so colour-mod turns
	// them into a flat fill of any colour. That is STAMPFILL_PRIM, which is
	// how the C draws a cloaking Ilwrath: not a faded sprite but a solid
	// silhouette stepping through a fixed colour ramp (ilwrath.c:250-285).
	// Tinting the real cels instead only darkens them, because colour-mod
	// multiplies -- a dark hull stays dark and the effect disappears.
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

// Loads `ani` and everything it names, relative to the .ani's own directory.
// Returns an empty ShipSprites if anything is missing -- content problems are
// reported by the browser and by the resource-graph check in CI, so there is
// nothing useful to say here that is not already said louder elsewhere.
// `colortable` is the global palette table (colortable.main, base/uqm.ct);
// cels name a slot in it rather than in any .ct beside them.
[[nodiscard]] SpriteSet loadSprites(platform::Platform &window,
		const std::filesystem::path &ani,
		const std::filesystem::path &colortable);

}  // namespace uqm::game

#endif  // UQM2_GAME_SPRITESET_HPP
