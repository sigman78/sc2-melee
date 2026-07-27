// Copyright the Ur-Quan Masters contributors. GPL-2.0-or-later.

#ifndef UQM2_GAME_MELEE_HPP
#define UQM2_GAME_MELEE_HPP

#include "engine/core/Types.hpp"

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
	std::string_view battleSounds;  // slots below, in the .snd's line order
};

// battle.snd, by line -- the C's BATTLE_SOUND_EFFECTS (sounds.h:31-38).
// The order is a content contract, written down once.
enum class BattleSound : usize
{
	GrabCrew = 0,      // unused until crew pickup exists
	ShipExplodes = 1,
	Damaged1 = 2,      // TARGET_DAMAGED_FOR_1_PT, then _2_3, _4_5 ...
	Damaged23 = 3,
	Damaged45 = 4,
	Damaged6Plus = 5,  // ... and _6_PLUS_PT, the loudest and the cap
};

[[nodiscard]] constexpr usize
slot(BattleSound s) noexcept
{
	return static_cast<usize>(s);
}

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
