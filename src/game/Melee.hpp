// Copyright the Ur-Quan Masters contributors. GPL-2.0-or-later.

#ifndef UQM2_GAME_MELEE_HPP
#define UQM2_GAME_MELEE_HPP

#include <string_view>

namespace uqm::game {

// The melee mode's non-ship content, by resource id -- what the arena
// itself draws and plays, as opposed to what a ShipDef brings (Ships.hpp).
struct MeleeArt
{
	std::string_view asteroid;
	std::string_view blast;  // a weapon going off
	std::string_view boom;   // an asteroid coming apart

	// The C picks a planet type at random per battle (load_gravity_well,
	// cons_res.c:52-82). One fixed type until melee setup exists to choose.
	std::string_view planet;

	std::string_view stars;
	std::string_view battleSounds;  // getcrew, shipdies, then the booms
};

inline constexpr MeleeArt kMeleeArt{
	.asteroid = "graphics.asteroid.large",
	.blast = "graphics.blast.large",
	.boom = "graphics.boom.large",
	.planet = "planet.acid.large",
	.stars = "graphics.stars",
	.battleSounds = "sounds.battle",
};

}  // namespace uqm::game

#endif  // UQM2_GAME_MELEE_HPP
