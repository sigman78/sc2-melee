// Copyright the Ur-Quan Masters contributors. GPL-2.0-or-later.

#include "game/Ships.hpp"

#include "sim/ships/Human.hpp"
#include "sim/ships/Ilwrath.hpp"

#include <array>

namespace uqm::game {

std::span<const ShipDef> shipCatalog() noexcept
{
	// Function-local so the sim's spec statics are constructed before they
	// are borrowed, whichever translation unit asks first.
	static const std::array<ShipDef, 2> defs{{
			{
					.key = "earthling.cruiser",
					.spec = &sim::earthlingCruiser(),
					.art{
							.ship = "ship.earthling.graphics.human.large",
							.weapon = "ship.earthling.graphics.saturn.large",
							.sounds = "ship.earthling.sounds",
					},
			},
			{
					.key = "ilwrath.avenger",
					.spec = &sim::ilwrathAvenger(),
					.art{
							.ship = "ship.ilwrath.graphics.avenger.large",
							.weapon = "ship.ilwrath.graphics.fire.large",
							.sounds = "ship.ilwrath.sounds",
					},
			},
	}};
	return defs;
}

Borrowed<const ShipDef> findShip(std::string_view key) noexcept
{
	for (const ShipDef &def : shipCatalog())
		if (def.key == key)
			return &def;
	return nullptr;
}

}  // namespace uqm::game
