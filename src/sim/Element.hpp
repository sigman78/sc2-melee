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

// Traits as types: any(flags & X) became registry().all_of<X>(id), and the
// bitfield stopped filling up (review-002 called it "close to full" at 13).
// FiniteLife and Disappearing, the last two ELEMENT_FLAGS bits, are gone the
// same way (review-007 W3): Lifetime and Doomed (Entity.hpp).

// A player's ship, as opposed to a projectile or a rock.
comp struct PlayerShip
{
};

// GRAVITY_MASS (element.h:198) is `mass > 100`; gravity.c/collide.c ask
// `mass + 1 > 100` instead (gravity.c:34,45, collide.c:102,139) -- exempting
// a fleeing ship (battle.c:92) from gravity/impulse but not damage (misc.c:214).
inline constexpr i32 kMaxShipMass = 10;              // element.h:197
inline constexpr i32 kGravityMass = kMaxShipMass * 10;  // 100

// GRAVITY_MASS as written: does this push instead of being pushed?
[[nodiscard]] constexpr bool
isGravityMass(i32 massPoints) noexcept
{
	return massPoints > kGravityMass;
}

// GRAVITY_MASS as gravity.c asks it. See above for why they differ.
[[nodiscard]] constexpr bool
isGravitySource(i32 massPoints) noexcept
{
	return massPoints + 1 > kGravityMass;
}

// Element's mass, split out (review-007 W4a): the plan table missed this
// one, named here after the user's own sketch. Every collidable thing has
// one -- collisionPossible's both-massless skip, Impulse's denominators and
// isGravityMass/isGravitySource all read it without needing anything else
// off Element.
comp struct Physique
{
	i32 mass = 0;
};

// Element{hitPoints}, split out (review-007 W4b): attached only where read
// (the minimal-composition rule) -- weapons (piercing threshold, and the
// hit-point-to-zero death Damage.cpp deals in), the asteroid field, and the
// planet. A ship's toughness is its crew (ShipState), never this: a
// crewed hull never gets a Vitality, and every doDamage/killOverlapSpawn
// site that touches it already branches on ship-vs-not first.
comp struct Vitality
{
	i32 hitPoints = 0;
};

// Element{playerNr, owner}, split out (review-007 W4b) -- the one
// deliberately uniform attach in this stage: every spawn gets one
// (Battle::spawn/spawnBeam), never omitted. Too many readers to minimise
// (events, targeting, the sweep, IgnoreSimilar pairing, the app's colour
// and sound dispatch), and the defaults -- nobody's playerNr, nobody's
// owner -- are meaningful values in their own right, not placeholders for
// an absent component.
comp struct Allegiance
{
	// -1 for things nobody owns, like asteroids.
	i32 playerNr = -1;

	// The ship this came from: pParent in the C (element.h:192); a ship owns
	// itself. IGNORE_SIMILAR skips a pair sharing an owner (stops a flame
	// burning its own ship) -- owner, not player, so allied ships still collide.
	EntityId owner = kNoEntity;
};

// Element{colorCycle}, split out (review-007 W4b): attached only where
// read, which is weapons alone -- Draw.cpp's ByFrame policy is the cel a
// shot draws (a directional missile's facing, or the flame's growth
// frame), and the Ilwrath flame reads/advances the same value for its own
// mask lookup. Debris/IonTrail/ShipShadow animate by Lifetime::remaining
// instead (RampPoint/RampSilhouette/DebrisFrames key off age, not this),
// and a dying ship draws by facing (ByFacing) -- both wrote a colorCycle of
// 0 on the old Element that nothing ever read back.
comp struct AnimFrame
{
	i32 n = 0;
};

// The Ilwrath flame's own animate-pass concept, generalised (review-007
// W5): a shot whose AnimFrame advances every frame it lives, its collision
// silhouette following the growth (ilwrath.c:126-139) -- unlike a
// directional missile, whose frame follows its facing instead (Human.cpp's
// guidedShotPreProcess). Stamped from WeaponSpec::frameDriven at fire time;
// the mask source (WeaponGuidance) and the frame itself (AnimFrame) already
// exist, so this is a pure marker.
comp struct FrameDriven
{
};

// What kind of thing this is: a real tag, not a frame-pointer comparison
// (cyborg.c:1222-1227) or a cross-ship header include for constants
// (shofixti.c:251-253 pulls in orz.h to recognise a turret).
enum class ElementKind : u8
{
	Unknown = 0,
	Ship,
	Weapon,
	Asteroid,
	Planet,
	Blast,
	Turret,

	// A beam: Beam{from,to} is its two *ends*, not before/after positions --
	// matching the C's LINE_PRIM reuse of one element for it (weapon.c:44-85).
	Laser,

	// A single point of a ship's exhaust, and the shadow a ship leaves while
	// warping in. One element in the C too -- both use cycle_ion_trail
	// (tactrans.c:756-790), which is why they share a kind here.
	IonTrail,

	// A fading silhouette shed while warping in: ship-shaped, not a point --
	// the C draws it as a STAMPFILL_PRIM (tactrans.c:893-930), sharing the ion
	// trail's colour ramp but not its geometry.
	ShipShadow,

	// One spark of a dying ship. The explosion is a *swarm* of these thrown
	// off over 26 frames while the hull is still there (tactrans.c:542-615),
	// not one animation played on the wreck.
	Debris,
};

struct Element
{
	// Stable addresses: Battle::get()'s raw pointer is held across nested
	// calls (resolveAgainst's own recursion, killOverlapSpawn's doDamage),
	// which the old arena guaranteed and entt's default swap-and-pop storage
	// does not.
	static constexpr auto in_place_delete = true;

	ElementKind kind = ElementKind::Unknown;

	// The silhouette/facing this element entered the frame with is NOT here:
	// it is the overlap-repair protocol's own scratch (process.c:453-506),
	// so it lives as a component private to Battle.cpp (review-004 X5).
	//
	// Everything else Element once carried -- flags, kind-specific fields,
	// preProcess/onDeath/onCollision hooks, collidedWith -- has moved to its
	// own component or a dispatch keyed on component presence (review-007
	// W3 through W5). What is left is what this file's name still means:
	// which kind of thing this is.
};

}  // namespace uqm::sim

#undef comp

#endif  // UQM2_SIM_ELEMENT_HPP
