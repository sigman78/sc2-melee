// Copyright the Ur-Quan Masters contributors. GPL-2.0-or-later.

#ifndef UQM2_SIM_SPECIALS_HPP
#define UQM2_SIM_SPECIALS_HPP

#include "sim/Entity.hpp"

namespace uqm::sim {

class Battle;

// What a ship's SPECIAL does, as mechanics rather than per-ship hooks.
//
// The uniform half of a special is a cost and a debounce, and that stays on
// SpecialSpec: the engine only ticks the counter (ship.c:342-346). The
// effect is what varies, and it is a *component* the ship is equipped with
// (ShipSpec::equip) plus a step over it. Nothing here names a ship, and a
// ship that reuses a mechanic costs nothing outside its own file.
//
// The two entry points below are the roster: adding a mechanic is a
// component, a step, and one line in the matching dispatcher.

// The pre-turn slot, run from ShipMachines: what the C puts in
// RACE_DESC.preprocess_func (ship.c:232-236). The cloak lives here because
// it has to win the energy race against the same frame's shot.
void runPreTurnSpecials(Battle &b, EntityId id) noexcept;

// The post-fire slot, run from the gate that spent the debounce
// (ship.c:342-346).
void runGatedSpecials(Battle &b, EntityId id) noexcept;

// The cloak machine (ilwrath.c:232-394): walks Cloak::level, keeps the
// Cloaked tag in sync with it, and snaps the ambush shot. Sole writer of
// both, which is why every other reader just asks has<Cloaked>.
void cloakStep(Battle &b, EntityId id) noexcept;

// Point defence (human.c:203-236): burns every shot in range, paying once
// for the volley. Reads PointDefence::range.
void pointDefenceStep(Battle &b, EntityId id) noexcept;

}  // namespace uqm::sim

#endif  // UQM2_SIM_SPECIALS_HPP
