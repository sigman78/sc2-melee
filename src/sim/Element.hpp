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

namespace uqm::sim {

class Battle;

// Traits as types: any(flags & X) became registry().all_of<X>(id), and the
// bitfield stopped filling up (review-002 called it "close to full" at 13).
// FiniteLife and Disappearing, the last two ELEMENT_FLAGS bits, are gone the
// same way (review-007 W3): Lifetime and Doomed (Entity.hpp).

// A player's ship, as opposed to a projectile or a rock.
struct PlayerShip
{
};

// Skips velocity integration. IGNORE_VELOCITY in the C, and distinct from
// DefyPhysics -- process.c:163 tests this one and nothing else when
// deciding whether an element moves.
struct IgnoreVelocity
{
};

// `current` and `next` are the two ENDS of a beam, not motion -- the C's
// LINE_PRIM elements (weapon.c:44-85). The step neither seeds next from
// current at spawn nor commits current = next.
struct BeamGeometry
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

	// A beam: `current`/`next` are its two *ends*, not before/after positions --
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

// Free functions taking the battle and the element's id: no captured state,
// so they can't outlive it. The C mutates per-instance hooks at runtime
// (chmmr.c:773 deletes its hook, pkunk.c:282 reinstalls one); this doesn't.
using ElementHook = void (*)(Battle &, EntityId) noexcept;

struct Element
{
	// Stable addresses: hooks hold a pointer to their own element across
	// spawns (ShipSystems.cpp's weapon loop), which the old arena guaranteed
	// and entt's default swap-and-pop storage does not.
	static constexpr auto in_place_delete = true;

	// Where it is now, and where the step is taking it.
	Vec2i current;
	Vec2i next;

	Velocity velocity;

	ElementKind kind = ElementKind::Unknown;

	// -1 for things nobody owns, like asteroids.
	i32 playerNr = -1;

	Facing facing;

	i32 hitPoints = 0;
	i32 mass = 0;
	i32 damage = 0;

	// How far along its travel direction a weapon's blast sits, in display
	// pixels, so the explosion lands on the surface it hit rather than inside
	// it (weapon.c:202-208).
	i32 blastOffset = 0;

	// Where an element is in its colour/frame sequence (ion trail fade,
	// explosion frames). The C's colorCycleIndex, kept per-element for the
	// same reason: the alternative is a parallel table keyed by entity.
	i32 colorCycle = 0;

	// Frames until the ship may turn or thrust again. A collision adds to
	// both, which is the stagger you feel after hitting something
	// (collide.c:113-116).
	i32 turnWait = 0;
	i32 thrustWait = 0;

	// The silhouette/facing this element entered the frame with is NOT here:
	// it is the overlap-repair protocol's own scratch (process.c:453-506),
	// so it lives as a component private to Battle.cpp (review-004 X5) --
	// hooks cannot even name it, let alone corrupt it.

	ElementHook preProcess = nullptr;
	ElementHook onDeath = nullptr;

	// Runs after a collision has been resolved, on each side, with
	// `collidedWith` already set. This is collision_func in the C.
	ElementHook onCollision = nullptr;

	// What this element hit, valid only inside a collision hook.
	EntityId collidedWith = kNoEntity;

	// The ship this came from: pParent in the C (element.h:192); a ship owns
	// itself. IGNORE_SIMILAR skips a pair sharing an owner (stops a flame
	// burning its own ship) -- owner, not player, so allied ships still collide.
	EntityId owner = kNoEntity;
};

}  // namespace uqm::sim

#endif  // UQM2_SIM_ELEMENT_HPP
