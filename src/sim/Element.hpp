// Copyright the Ur-Quan Masters contributors. GPL-2.0-or-later.

#ifndef UQM2_SIM_ELEMENT_HPP
#define UQM2_SIM_ELEMENT_HPP

#include "engine/core/Borrowed.hpp"
#include "engine/core/Geometry.hpp"
#include "engine/core/Types.hpp"
#include "sim/Collision.hpp"
#include "sim/Entity.hpp"
#include "sim/Trig.hpp"
#include "sim/Velocity.hpp"

#define comp

namespace uqm::sim {

// GRAVITY_MASS (element.h:198) is `mass > 100`; gravity.c/collide.c ask
// `mass + 1 > 100` instead (gravity.c:34,45, collide.c:102,139) -- exempting
// a fleeing ship (battle.c:92) from gravity/impulse but not damage
// (misc.c:214).
inline constexpr i32 kMaxShipMass = 10;                 // element.h:197
inline constexpr i32 kGravityMass = kMaxShipMass * 10;  // 100

// GRAVITY_MASS as written: does this push instead of being pushed?
[[nodiscard]] constexpr bool isGravityMass(i32 massPoints) noexcept
{
	return massPoints > kGravityMass;
}

// GRAVITY_MASS as gravity.c asks it. See above for why they differ.
[[nodiscard]] constexpr bool isGravitySource(i32 massPoints) noexcept
{
	return massPoints + 1 > kGravityMass;
}

// A body's mass. Every collidable thing has one -- collisionPossible's
// both-massless skip, Impulse's denominators, and isGravityMass/
// isGravitySource all read it.
comp struct Physique
{
	i32 mass = 0;
};

// Hit points, attached only where read: weapons (piercing threshold, and the
// hit-point-to-zero death Damage.cpp deals), the asteroid field, and the
// planet. A crewed hull's toughness is its crew (ShipState); it never gets one.
comp struct Vitality
{
	i32 hitPoints = 0;
};

// Every spawn gets one (Battle::spawn/spawnBeam), never omitted -- too many
// readers (events, targeting, IgnoreSimilar pairing, colour/sound dispatch)
// to minimise, and the defaults are meaningful values, not placeholders.
comp struct Allegiance
{
	// -1 for things nobody owns, like asteroids.
	i32 playerNr = -1;

	// The ship this came from: pParent in the C (element.h:192); a ship owns
	// itself. IGNORE_SIMILAR skips a pair sharing an owner (stops a flame
	// burning its own ship) -- owner, not player, so allied ships still
	// collide.
	EntityId owner = kNoEntity;
};

// A cel index: weapons alone carry one -- Draw.cpp's ByFrame policy for a
// shot's facing/growth frame, and the Ilwrath flame's own mask lookup.
// Debris/IonTrail/ShipShadow animate by Lifetime::remaining instead.
comp struct AnimFrame
{
	i32 n = 0;
};

// A shot whose AnimFrame advances every frame it lives, its collision
// silhouette following the growth (ilwrath.c:126-139) -- unlike a directional
// missile, whose frame follows its facing (Human.cpp's guidedShotPreProcess).
comp struct FrameDriven
{};

}  // namespace uqm::sim

#undef comp

#endif  // UQM2_SIM_ELEMENT_HPP
