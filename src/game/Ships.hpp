// Copyright the Ur-Quan Masters contributors. GPL-2.0-or-later.

#ifndef UQM2_GAME_SHIPS_HPP
#define UQM2_GAME_SHIPS_HPP

#include "engine/core/Borrowed.hpp"
#include "engine/core/Types.hpp"
#include "sim/Ship.hpp"

#include <span>
#include <string_view>

namespace uqm::platform {
class Platform;
}

namespace uqm::game {

class Resources;

// The resource ids a ship's presentation loads -- the C's SHIP_DATA
// (ship.h:69-81), minus the three sizes collapsed to `-big` and the
// pieces melee does not draw yet. sim/ never sees these: art lives in game/.
struct ShipArt
{
	std::string_view ship;    // GFXRES: one cel per facing
	std::string_view weapon;  // GFXRES: the primary's projectile frames
	std::string_view sounds;  // SNDRES: slots in the .snd's line order
};

// A ship's .snd, by line: the order is a content contract, so it is
// written down once instead of as a bare index at each play site.
enum class ShipSound : usize
{
	Primary = 0,    // the main weapon firing
	Secondary = 1,  // the SPECIAL -- the Cruiser's point-defence laser
};

[[nodiscard]] constexpr usize slot(ShipSound s) noexcept
{
	return static_cast<usize>(s);
}

// A ship type as one entry: the key setup picks it by, the sim descriptor,
// and the content the descriptor's presentation needs. A code literal today,
// the shape a ship file parses into later -- data all the way down.
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

// The spec a battle flies: def.spec plus content-derived facing/weapon
// masks. Spans point into `content`'s caches, which outlive the battle
// (Resources.hpp, LIFETIME). In Materialize.cpp/uqm2_platform: needs a window.
[[nodiscard]] sim::ShipSpec materialize(
		const ShipDef &def, Resources &content, platform::Platform &window);

}  // namespace uqm::game

#endif  // UQM2_GAME_SHIPS_HPP
