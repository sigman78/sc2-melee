// Copyright the Ur-Quan Masters contributors. GPL-2.0-or-later.

#include "game/Resources.hpp"
#include "game/Ships.hpp"

namespace uqm::game {

sim::ShipSpec
materialize(const ShipDef &def, Resources &content,
		platform::Platform &window)
{
	sim::ShipSpec spec = *def.spec;
	// Never empty: missing art yields Resources' placeholder, whose one
	// block mask serves every facing -- the ship still flies, as a shape.
	spec.facingMasks = content.sprites(window, def.art.ship).masks;
	spec.weapon.masks = content.sprites(window, def.art.weapon).masks;
	return spec;
}

}  // namespace uqm::game
