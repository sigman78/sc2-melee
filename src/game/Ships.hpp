// Copyright the Ur-Quan Masters contributors. GPL-2.0-or-later.

#ifndef UQM2_GAME_SHIPS_HPP
#define UQM2_GAME_SHIPS_HPP

#include "engine/core/Borrowed.hpp"
#include "sim/Ship.hpp"

#include <span>
#include <string_view>

namespace uqm::platform {
class Platform;
}

namespace uqm::game {

class Resources;

// The resource ids a ship's presentation loads -- the C's SHIP_DATA
// (ship.h:69-81), minus the three sizes collapsed to `-big` (SpriteSet.hpp)
// and the pieces melee does not draw yet (captain window, victory ditty).
// sim/ never sees these: art binds to a spec in game/, not inside it.
struct ShipArt
{
	std::string_view ship;    // GFXRES: one cel per facing
	std::string_view weapon;  // GFXRES: the primary's projectile frames
	std::string_view sounds;  // SNDRES: slots in the .snd's line order
};

// A ship type as one entry: the key setup picks it by, the sim descriptor,
// and the content the descriptor's presentation needs. A code literal today,
// the shape a ship file parses into later (review-002 §3) -- which is why it
// is data all the way down and not a class.
struct ShipDef
{
	std::string_view key;  // "earthling.cruiser" -- ours, not a uqm.rmp id
	Borrowed<const sim::ShipSpec> spec = nullptr;
	ShipArt art;
};

// Every ship the game knows. Two entries in M1; M2 grows it one literal per
// ship, touching no presentation code.
[[nodiscard]] std::span<const ShipDef> shipCatalog() noexcept;

// The catalog entry a key names, or null -- a roster will one day come from
// data, so an unknown key is the caller's question to ask.
[[nodiscard]] Borrowed<const ShipDef> findShip(std::string_view key) noexcept;

// The spec copy a battle flies: def.spec plus the content-derived facing
// and weapon masks. The spans point into `content`'s caches, which outlive
// any battle (Resources.hpp, LIFETIME). Defined in Materialize.cpp, which
// lives in uqm2_platform -- loading sprites means a window.
[[nodiscard]] sim::ShipSpec materialize(const ShipDef &def,
		Resources &content, platform::Platform &window);

}  // namespace uqm::game

#endif  // UQM2_GAME_SHIPS_HPP
