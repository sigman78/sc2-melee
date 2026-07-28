// Copyright the Ur-Quan Masters contributors. GPL-2.0-or-later.

#ifndef UQM2_APP_MELEE_DRAW_HPP
#define UQM2_APP_MELEE_DRAW_HPP

#include "engine/core/Types.hpp"
#include "sim/Element.hpp"

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

// How an attached Visual becomes pixels. Chosen once at spawn (visualFor);
// draw() only dispatches on it.
enum class CelPolicy : u8
{
	ByFacing,        // ships, asteroids, planet, blasts: cel = facing.raw() % frames
	ByFrame,         // weapons: cel = AnimFrame.n % frames
	RampPoint,       // ion trail: 1px kIonRamp[age] dot
	RampSilhouette,  // warp shadow: silhouette tinted kIonRamp[age]
	DebrisFrames,    // dying-ship spark: boom frames stepped by age
	BeamLine,        // laser: line Beam.from->to
	Rect,            // fallback: fillRect in `fallback` colour
};

// What one element draws as. Attached once, at spawn -- see visualFor --
// rather than deduced from ElementKind every frame.
//
// A component in the battle's registry, emplaced by the app: the sim never
// names this type, ownership is by component type, not by store (review-004
// X3). Reaping the entity reaps its Visual -- the old RenderStore and its
// purgeDead are gone.
comp struct Visual
{
	const game::SpriteSet *sprites = nullptr;  // null for RampPoint/BeamLine/Rect
	CelPolicy policy = CelPolicy::Rect;
	Colour fallback{0xC0, 0xC0, 0xC0};
};

// THE attach-time dispatch: the only place left that switches on
// ElementKind, besides the sound code. Built once per spawned element.
// Art comes from the owner's roster entry or kMeleeArt, resolved through
// Resources' cache -- which is why the Game is not const here.
[[nodiscard]] Visual visualFor(
		Game &g, sim::ElementKind kind, i32 playerNr);

// Renders one frame: the starfield, every element, the HUD, and -- if
// toggled -- the collision debug overlay.
void draw(Game &g);

}  // namespace uqm::melee

#undef comp

#endif  // UQM2_APP_MELEE_DRAW_HPP
