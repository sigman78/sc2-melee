// Copyright the Ur-Quan Masters contributors. GPL-2.0-or-later.

#include "app/melee/Sound.hpp"
#include "app/melee/Game.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>

namespace uqm::melee {

void
playStepSounds(Game &g)
{
	// Which boom plays tracks hit strength: TARGET_DAMAGED_FOR_1_PT +
	// (damage >> 1), capped at slot 6 (weapon.c:168-172, ship.c:369-371);
	// battle.snd orders booms after getcrew/shipdies.
	for (const sim::CollisionEvent &c : g.battle.collisions())
	{
		std::int32_t damage = 0;
		for (const sim::EntityId side : {c.a, c.b})
			if (const auto el = g.battle.get(side); el != nullptr)
				damage = std::max(damage, el->damage);

		const std::size_t slot = std::min<std::size_t>(
				kBoomFirstSlot + static_cast<std::size_t>(damage >> 1),
				kBoomFirstSlot + 3);
		if (g.battleSounds.size() > slot)
			g.audio.play(g.battleSounds[slot], kEffectGain);
	}

	// Weapons fired this frame, and beams: read from step()'s own spawn
	// events (design-notes.md D5), the same shape the collision sounds use.
	for (const sim::SpawnEvent &sp : g.battle.spawns())
	{
		if (sp.kind == sim::ElementKind::Weapon)
		{
			// Whose weapon: the Cruiser's nuke and the Avenger's flame are
			// different sounds, both slot 0 of their own ship's .snd.
			const auto &set = sp.playerNr == 0 ? g.cruiserSounds
											   : g.avengerSounds;
			if (!set.empty())
				g.audio.play(set[0], kEffectGain);
		}
		else if (sp.kind == sim::ElementKind::Laser)
		{
			// cruiser.snd slot 1: secondary.wav, the point-defence laser
			// (human.c:232-234).
			if (g.cruiserSounds.size() > 1)
				g.audio.play(g.cruiserSounds[1], kEffectGain);
		}
	}

	// The explosion sound plays when it starts, not when the wreck is
	// reaped: StartShipExplosion fires SHIP_EXPLODES as it starts burning
	// (tactrans.c:722-727), 36 frames before the wreck disappears.
	for (std::size_t p = 0; p < g.ships.size(); ++p)
	{
		if (g.deathAnnounced[p])
			continue;
		auto s = g.battle.ship(g.ships[p]);
		if (s == nullptr || s->crew > 0)
			continue;
		g.deathAnnounced[p] = true;
		// battle.snd slot 1: shipdies.wav (tactrans.c:723-726).
		if (g.battleSounds.size() > 1)
			g.audio.play(g.battleSounds[1], kEffectGain);
	}
}

}  // namespace uqm::melee
