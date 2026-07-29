// Copyright the Ur-Quan Masters contributors. GPL-2.0-or-later.

#ifndef UQM2_APP_MELEE_DRAW_HPP
#define UQM2_APP_MELEE_DRAW_HPP

#include "engine/core/Geometry.hpp"
#include "engine/core/Types.hpp"
#include "sim/Battle.hpp"
#include "sim/Element.hpp"
#include "sim/World.hpp"

#include <array>
#include <numeric>

namespace uqm::game {
struct SpriteSet;
}

namespace uqm::melee {

struct Game;

struct Colour
{
	u8 r, g, b;
};

// A colour packed as 0xRRGGBB, the form the C's own tables are quoted in.
constexpr Colour rgb(u32 c) noexcept
{
	return Colour{static_cast<u8>((c >> 16) & 0xFF),
			static_cast<u8>((c >> 8) & 0xFF), static_cast<u8>(c & 0xFF)};
}

namespace comp {

// What one element draws as: a sprite set (null for a line/point effect
// with no art) and a fallback colour for when the set failed to load or
// none applies. The pass owns technique -- cel indexing, hotspot, tinting.
struct Visual
{
	const game::SpriteSet *sprites = nullptr;
	Colour fallback{0xC0, 0xC0, 0xC0};
};

}  // namespace comp

// What an element draws as, chosen from what it is composed of (ShipState/
// Shadow -> ship art, Warhead -> weapon art, ...). Built once per spawned
// element in setUp()/iterate(); resolved through Resources' cache.
[[nodiscard]] comp::Visual visualFor(Game &g, sim::EntityId id, i32 playerNr);

// The starfield: three planes (30/60/90 stars), each scrolling at
// 1/2^plane of the camera so nearer planes move faster (galaxy.c:37-44,
// 405-407); plane 0 is nearest -- biggest, brightest, fastest.
inline constexpr int kStarPlanes = 3;

// One parallax plane's star data.
struct StarPlane
{
	i32 count;

	// Fallback only, for when the art is missing: the cels carry their own
	// colour and brightness already graded by plane, which is why they draw
	// as frames rather than silhouettes.
	Colour colour;

	// Cel per plane -- 11x11 near, 5x5 mid, a pixel far, one size down from
	// star_frame_ofs's ordering (galaxy.c:316): the 11x11 cel is too large
	// for a background at this resolution.
	usize cel;
};

inline constexpr std::array<StarPlane, kStarPlanes> kStarsPerPlane{{
		{30, rgb(0x949CFC), 0},
		{60, rgb(0x808CFC), 2},
		{90, rgb(0xA4ACFC), 2},
}};

// Total stars across all three parallax planes, folded from kStarsPerPlane
// so the two cannot disagree.
inline constexpr int kStarCount =
		std::accumulate(kStarsPerPlane.begin(), kStarsPerPlane.end(), 0,
				[](int sum, const StarPlane &p) { return sum + p.count; });

// How large a patch the field tiles over, in display pixels. The C varies
// on-screen density with zoom (galaxy.c:248-259); this field is
// zoom-independent, so it tiles over four screens for ~45 stars in view.
inline constexpr Extent2i kStarField = sim::kSpace * 2;

namespace comp {

// One entity with no Position or Order (Battle::create(), outside the sim's
// element count), populated at setup, read only by renderStars. Positions
// are display pixels on a per-plane torus (galaxy.c:37-44).
struct Starfield
{
	std::array<Vec2i, kStarCount> stars{};
};

// A collision event held on screen for kLife frames. App-owned (Battle::
// create()), reaped by age in iterate() via Battle::destroy() -- never a
// sim element, so it carries no Order and is invisible to eachOrdered.
struct Mark
{
	// How long a contact point stays on screen, in simulation frames.
	static constexpr u64 kLife = 24;

	sim::CollisionEvent event;
	u64 bornFrame = 0;
};

}  // namespace comp

// Renders one frame: the ordered pipeline of semantic passes -- stars,
// planet, asteroids, ships, projectiles, effects, marks, the HUD and,
// ctx-gated, the collision debug overlay.
void draw(Game &g);

}  // namespace uqm::melee

#endif  // UQM2_APP_MELEE_DRAW_HPP
