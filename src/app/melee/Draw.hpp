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

#define comp

namespace uqm::melee {

struct Game;

struct Colour
{
	u8 r, g, b;
};

// A colour packed as 0xRRGGBB, the form the C's own tables are quoted in.
constexpr Colour
rgb(u32 c) noexcept
{
	return Colour{static_cast<u8>((c >> 16) & 0xFF),
			static_cast<u8>((c >> 8) & 0xFF),
			static_cast<u8>(c & 0xFF)};
}

// What one element draws as: a sprite set (null for a line/point effect with
// no art) and a fallback colour for when the set failed to load or none
// applies. The pass owns how it is drawn -- cel indexing, hotspot placement,
// tinting -- so this carries nothing about technique any more (review-007
// W7: CelPolicy retires, the pass IS the policy). A component in the
// battle's registry, emplaced by the app: the sim never names this type,
// ownership is by component type, not by store (review-004 X3).
comp struct Visual
{
	const game::SpriteSet *sprites = nullptr;
	Colour fallback{0xC0, 0xC0, 0xC0};
};

// The attach-time art selection (review-007 W7): what an element draws as,
// chosen from what it is composed of -- a ShipState or Shadow tag means the
// owner's ship art, a Warhead means the owner's weapon art, and so on down
// to the plain Rect fallback. Ownership of "what kind of thing this is"
// belongs entirely to composition. Built once per spawned element, in
// setUp() and iterate() -- Art comes from the owner's roster entry or
// kMeleeArt, resolved through Resources' cache, which is why the Game is
// not const here.
[[nodiscard]] Visual visualFor(Game &g, sim::EntityId id, i32 playerNr);

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
inline constexpr int kStarCount = std::accumulate(kStarsPerPlane.begin(),
		kStarsPerPlane.end(), 0,
		[](int sum, const StarPlane &p) { return sum + p.count; });

// How large a patch the field tiles over, in display pixels. The C varies
// on-screen density with zoom (galaxy.c:248-259); this field is
// zoom-independent, so it tiles over four screens for ~45 stars in view.
inline constexpr Extent2i kStarField = sim::kSpace * 2;

// The starfield, as one entity (review-007 §3): not 180 star entities --
// component granularity follows behaviour granularity, and the field
// scrolls as one thing. Positions are in display pixels on a screen-sized
// torus per plane (galaxy.c:37-44); see renderStars. No Position, no
// Order: created once via Battle::create() (a bare entity, outside the
// sim's element count and its ordered walk), populated at battle setup,
// read every frame by renderStars alone.
comp struct Starfield
{
	std::array<Vec2i, kStarCount> stars{};
};

// A collision event, held on screen for kLife frames -- one frame at
// 24 Hz is not long enough to see (review-007 §3). An app-owned bare
// entity (Battle::create()), drawn by the marks pass and reaped by age in
// iterate() via Battle::destroy(); never a sim element, so it carries no
// Order and is invisible to eachOrdered.
comp struct Mark
{
	// How long a contact point stays on screen, in simulation frames.
	static constexpr u64 kLife = 24;

	sim::CollisionEvent event;
	u64 bornFrame = 0;
};

// Renders one frame: the ordered pipeline of semantic passes -- stars,
// planet, asteroids, ships, projectiles, effects, marks, the HUD and,
// ctx-gated, the collision debug overlay.
void draw(Game &g);

}  // namespace uqm::melee

#undef comp

#endif  // UQM2_APP_MELEE_DRAW_HPP
