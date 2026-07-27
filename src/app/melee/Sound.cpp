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
	// And heard. This is the second consumer the events were recorded
	// for rather than merely acted on -- the overlay was the first.
	//
	// Which boom depends on how hard the hit was: the C picks
	// TARGET_DAMAGED_FOR_1_PT + (damage >> 1), capped at the 6-plus
	// slot (weapon.c:168-172, ship.c:369-371). battle.snd lists them
	// in that order after getcrew and shipdies, so slot 2 is a scratch
	// and slot 5 is a nuke going off. Playing slot 2 for everything,
	// which is what this did first, makes every impact sound trivial.
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

	// Weapons fired this frame, and beams -- read from the step's own
	// spawn events, the same shape the collision sounds use. Scanning
	// the list for Appearing flags, which is what this did first, only
	// worked because a step-loop bug left the flag set one frame late;
	// the fixed loop clears it inside step().
	for (const sim::SpawnEvent &sp : g.battle.spawns())
	{
		if (sp.kind == sim::ElementKind::Weapon)
		{
			// Whose weapon: the Cruiser's nuke and the Avenger's flame are
			// different sounds, and both are slot 0 of their own ship's
			// .snd. Playing the Cruiser's for everything made the flame
			// sound like a missile launch every frame.
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

	// The explosion is announced when it *starts*, not when the wreck is
	// finally reaped. StartShipExplosion plays SHIP_EXPLODES as it sets the
	// element burning (tactrans.c:722-727), and the burn lasts 36 frames -- so
	// keying the sound off the element disappearing put it a second and a half
	// late, after the sparks had already gone out.
	for (std::size_t p = 0; p < g.ships.size(); ++p)
	{
		if (g.deathAnnounced[p])
			continue;
		auto s = g.battle.get(g.ships[p]);
		if (s == nullptr || s->ship.crew > 0)
			continue;
		g.deathAnnounced[p] = true;
		// battle.snd slot 1: shipdies.wav (tactrans.c:723-726).
		if (g.battleSounds.size() > 1)
			g.audio.play(g.battleSounds[1], kEffectGain);
	}
}

}  // namespace uqm::melee
