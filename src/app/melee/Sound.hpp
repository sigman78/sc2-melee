// Copyright the Ur-Quan Masters contributors. GPL-2.0-or-later.

#ifndef UQM2_APP_MELEE_SOUND_HPP
#define UQM2_APP_MELEE_SOUND_HPP

#define comp

namespace uqm::melee {

struct Game;

// Whether a ship's death has already had its stinger played, so it plays
// once (review-007 §3): Game::deathAnnounced[2]'s tag replacement -- dies
// with the ship, survives a roster of more than two where a fixed-size
// array of booleans breaks.
comp struct AnnouncedDead
{
};

// Turns this step's collision and spawn events, and any ship newly at zero
// crew, into sound. Called once per simulation step, right after the step
// itself.
void playStepSounds(Game &g);

}  // namespace uqm::melee

#undef comp

#endif  // UQM2_APP_MELEE_SOUND_HPP
