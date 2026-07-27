// Copyright the Ur-Quan Masters contributors. GPL-2.0-or-later.

#include "app/melee/Sound.hpp"
#include "app/melee/Game.hpp"

#include "game/Melee.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <span>

namespace uqm::melee {

namespace {

// The owner's sound set, by definition. A lookup, not a load: Resources
// caches by id and loadAssets already warmed it.
[[nodiscard]] std::span<const platform::Sound>
shipSounds(Game &g, std::int32_t playerNr)
{
	if (playerNr < 0 || static_cast<std::size_t>(playerNr) >= g.roster.size()
			|| g.roster[static_cast<std::size_t>(playerNr)] == nullptr)
		return {};
	return g.content.sounds(g.audio,
			g.roster[static_cast<std::size_t>(playerNr)]->art.sounds);
}

}  // namespace

void
playStepSounds(Game &g)
{
	const std::span<const platform::Sound> battleSnd =
			g.content.sounds(g.audio, game::kMeleeArt.battleSounds);

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
		if (battleSnd.size() > slot)
			g.audio.play(battleSnd[slot], kEffectGain);
	}

	// Weapons fired this frame, and beams: read from step()'s own spawn
	// events (design-notes.md D5), the same shape the collision sounds use.
	for (const sim::SpawnEvent &sp : g.battle.spawns())
	{
		if (sp.kind == sim::ElementKind::Weapon)
		{
			// Whose weapon: the nuke and the flame are different sounds,
			// both slot 0 of their owner's own .snd.
			const auto set = shipSounds(g, sp.playerNr);
			if (!set.empty())
				g.audio.play(set[0], kEffectGain);
		}
		else if (sp.kind == sim::ElementKind::Laser)
		{
			// Slot 1 of the owner's .snd: secondary.wav, the point-defence
			// laser (human.c:232-234).
			const auto set = shipSounds(g, sp.playerNr);
			if (set.size() > 1)
				g.audio.play(set[1], kEffectGain);
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
		if (battleSnd.size() > 1)
			g.audio.play(battleSnd[1], kEffectGain);
	}
}

}  // namespace uqm::melee
