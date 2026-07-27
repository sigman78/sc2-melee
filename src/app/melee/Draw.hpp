// Copyright the Ur-Quan Masters contributors. GPL-2.0-or-later.

#ifndef UQM2_APP_MELEE_DRAW_HPP
#define UQM2_APP_MELEE_DRAW_HPP

namespace uqm::melee {

struct Game;

// Renders one frame: the starfield, every element, the HUD, and -- if
// toggled -- the collision debug overlay.
void draw(Game &g);

}  // namespace uqm::melee

#endif  // UQM2_APP_MELEE_DRAW_HPP
