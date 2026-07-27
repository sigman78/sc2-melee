// Copyright the Ur-Quan Masters contributors. GPL-2.0-or-later.

#ifndef UQM2_APP_MELEE_DRAW_HPP
#define UQM2_APP_MELEE_DRAW_HPP

#include "sim/Element.hpp"

#include <cstdint>
#include <utility>
#include <vector>

namespace uqm::game {
struct SpriteSet;
}

namespace uqm::sim {
class Battle;
}

namespace uqm::melee {

struct Game;

struct Colour
{
	std::uint8_t r, g, b;
};

// How an attached Visual becomes pixels. Chosen once at spawn (visualFor);
// draw() only dispatches on it.
enum class CelPolicy : std::uint8_t
{
	ByFacing,        // ships, asteroids, planet, blasts: cel = facing.raw() % frames
	ByFrame,         // weapons: cel = colorCycle % frames
	RampPoint,       // ion trail: 1px kIonRamp[age] dot
	RampSilhouette,  // warp shadow: silhouette tinted kIonRamp[age]
	DebrisFrames,    // dying-ship spark: boom frames stepped by age
	BeamLine,        // laser: line current->next
	Rect,            // fallback: fillRect in `fallback` colour
};

// What one element draws as. Attached once, at spawn -- see visualFor --
// rather than deduced from ElementKind every frame.
struct Visual
{
	const game::SpriteSet *sprites = nullptr;  // null for RampPoint/BeamLine/Rect
	CelPolicy policy = CelPolicy::Rect;
	Colour fallback{0xC0, 0xC0, 0xC0};
};

// App-owned, keyed by sim::EntityId: the sim carries no art, so presentation
// lives beside it rather than inside it -- same storage and lookup as
// Battle's own component sidecars (sim/Battle.hpp ships_/weaponSpecs_).
class RenderStore
{
public:
	void attach(sim::EntityId id, Visual v);
	[[nodiscard]] const Visual *find(sim::EntityId id) const noexcept;

	// Drops entries whose entity the battle has already reaped.
	void purgeDead(const sim::Battle &b);

private:
	std::vector<std::pair<sim::EntityId, Visual>> visuals_;
};

// THE attach-time dispatch: the only place left that switches on
// ElementKind, besides the sound code. Built once per spawned element.
[[nodiscard]] Visual visualFor(
		const Game &g, sim::ElementKind kind, std::int32_t playerNr);

// Renders one frame: the starfield, every element, the HUD, and -- if
// toggled -- the collision debug overlay.
void draw(Game &g);

}  // namespace uqm::melee

#endif  // UQM2_APP_MELEE_DRAW_HPP
