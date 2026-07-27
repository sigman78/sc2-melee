// Copyright the Ur-Quan Masters contributors. GPL-2.0-or-later.

#ifndef UQM2_SIM_ELEMENT_HPP
#define UQM2_SIM_ELEMENT_HPP

#include "engine/core/Borrowed.hpp"
#include "engine/core/Geometry.hpp"
#include "sim/Collision.hpp"
#include "sim/EntityList.hpp"
#include "sim/Trig.hpp"
#include "sim/Velocity.hpp"

#include <cstdint>

namespace uqm::sim {

class Battle;
struct ShipSpec;

// What the player is asking for this frame.
enum class ShipInput : std::uint8_t
{
	None = 0,
	Left = 1u << 0,
	Right = 1u << 1,
	Thrust = 1u << 2,
	Weapon = 1u << 3,
	Special = 1u << 4,
};

[[nodiscard]] constexpr ShipInput
operator|(ShipInput a, ShipInput b) noexcept
{
	return static_cast<ShipInput>(
			static_cast<std::uint8_t>(a) | static_cast<std::uint8_t>(b));
}
[[nodiscard]] constexpr ShipInput
operator&(ShipInput a, ShipInput b) noexcept
{
	return static_cast<ShipInput>(
			static_cast<std::uint8_t>(a) & static_cast<std::uint8_t>(b));
}
constexpr ShipInput &
operator|=(ShipInput &a, ShipInput b) noexcept
{
	return a = a | b;
}
[[nodiscard]] constexpr bool
any(ShipInput f) noexcept
{
	return static_cast<std::uint8_t>(f) != 0;
}

// A ship's mutable half. The C keeps this in STARSHIP beside the ELEMENT;
// here it rides on the element, because a ship *is* an element and a second
// lifetime to keep in step would be a second thing to get wrong.
struct ShipState
{
	Borrowed<const ShipSpec> spec = nullptr;
	ShipInput input = ShipInput::None;

	std::int32_t crew = 0;
	std::int32_t energy = 0;

	std::int32_t energyCounter = 0;
	std::int32_t weaponCounter = 0;
	std::int32_t specialCounter = 0;

	SpeedState speed = SpeedState::Normal;

	// SHIP_IN_GRAVITY_WELL (races.h:71). Its own bit in the C, orthogonal to
	// the at-max/beyond-max pair rather than a fourth value of them: while it
	// is set a ship may accelerate past its own maximum, up to the hard
	// kMaxAllowedSpeed ceiling (ship.c:82, 106-112). Gravity sets it, and the
	// next thrust clears it (ship.c:263-267).
	bool inGravityWell = false;

	// Where the ship is in the cloak's colour walk. This is the prim state of
	// ilwrath_preprocess, made an index:
	//
	//     0            STAMP -- solid, visible, machine idle
	//     1..5         STAMPFILL fills: white, cyan-white, dark cyan, blue,
	//                  dark blue (ilwrath.c:349-374 going in, 255-273 out)
	//     kCloakFullLevel (6)   BLACK -- fully cloaked
	//
	// The Ilwrath cloak is not a fade: the C walks this fixed sequence one
	// step per frame, and runs it backwards to uncloak. The walk itself, and
	// everything that reverses it, lives in ilwrathPreProcess (Ship.cpp) --
	// the direction is derived each frame from the inputs and the counter,
	// exactly as the C derives it from the prim colour, so there is no
	// separate "cloaking" bool to fall out of step with it.
	std::int32_t cloakLevel = 0;
};

// The cloak walk: five visible fill colours (levels 1..5), then black.
inline constexpr std::int32_t kCloakVisibleColours = 5;
inline constexpr std::int32_t kCloakFullLevel = kCloakVisibleColours + 1;

// What an element is doing this frame. The C keeps these in one
// ELEMENT_FLAGS word (element.h); the ones the step loop itself reasons about
// are here, and the rest belong to whoever owns the element.
enum class ElementFlags : std::uint32_t
{
	None = 0,

	// Created this frame. Its `next` has not been integrated yet, so the
	// step has to seed it from `current` rather than moving it.
	Appearing = 1u << 0,

	// Reaped at the end of this frame.
	Disappearing = 1u << 1,

	// Counts down `lifeSpan` and disappears at zero.
	FiniteLife = 1u << 2,

	// Takes part in no collisions at all.
	NonSolid = 1u << 3,

	// Does not collide with others sharing its owner -- a flame stream, so
	// it does not eat itself (ilwrath.c:203).
	IgnoreSimilar = 1u << 4,

	// Already collided this frame; the step will not test it again.
	Collided = 1u << 5,

	// Already preprocessed this frame. This is the flag that distinguishes
	// elements present at the start of the frame from ones spawned during
	// it, and it is load-bearing -- see Step.hpp.
	PreProcessed = 1u << 6,

	// Already postprocessed this frame.
	PostProcessed = 1u << 7,

	// Collision bookkeeping, not motion: set when two elements met with no
	// relative motion to exchange (collide.c:84-85). DEFY_PHYSICS in the C.
	DefyPhysics = 1u << 8,

	// Skips velocity integration. IGNORE_VELOCITY in the C, and distinct
	// from DefyPhysics -- process.c:163 tests this one and nothing else when
	// deciding whether an element moves.
	IgnoreVelocity = 1u << 10,

	// A player's ship, as opposed to a projectile or a rock.
	PlayerShip = 1u << 9,

	// OBJECT_CLOAKED. Invisible to weapon targeting (weapon.c:344) and to the
	// Cruiser's point defence (human.c:202), and drawn differently. It does
	// *not* stop collisions -- a cloaked Avenger still hits an asteroid.
	//
	// Set only at FULL black: OBJECT_CLOAKED in the C is "STAMPFILL and
	// BLACK" (element.h:201-204), so a ship is targetable through the whole
	// fade, in both directions. Setting it for any partial fade, which is
	// what this did first, made the Avenger missile-proof from the frame
	// SPECIAL landed -- a significant unmarked buff.
	Cloaked = 1u << 11,
};

[[nodiscard]] constexpr ElementFlags
operator|(ElementFlags a, ElementFlags b) noexcept
{
	return static_cast<ElementFlags>(
			static_cast<std::uint32_t>(a) | static_cast<std::uint32_t>(b));
}
[[nodiscard]] constexpr ElementFlags
operator&(ElementFlags a, ElementFlags b) noexcept
{
	return static_cast<ElementFlags>(
			static_cast<std::uint32_t>(a) & static_cast<std::uint32_t>(b));
}
[[nodiscard]] constexpr ElementFlags
operator~(ElementFlags a) noexcept
{
	return static_cast<ElementFlags>(~static_cast<std::uint32_t>(a));
}
constexpr ElementFlags &
operator|=(ElementFlags &a, ElementFlags b) noexcept
{
	return a = a | b;
}
constexpr ElementFlags &
operator&=(ElementFlags &a, ElementFlags b) noexcept
{
	return a = a & b;
}
[[nodiscard]] constexpr bool
any(ElementFlags f) noexcept
{
	return static_cast<std::uint32_t>(f) != 0;
}

// Mass, and the two different questions the C asks about it.
//
// GRAVITY_MASS (element.h:198) is `mass > MAX_SHIP_MASS * 10`, i.e. > 100.
// But gravity.c never calls it on the mass itself -- always on `mass + 1`
// (gravity.c:34,45). The off-by-one is deliberate and it is not about planets,
// which have mass 200 (misc.c:53,71) and clear either test.
//
// It is about a ship running away. DoRunAway sets mass_points to exactly 100
// (battle.c:92), which fails `> 100` but passes `+ 1 > 100`. So a fleeing ship
// reads as a gravity *source* to gravity.c -- and because gravity skips any
// pair whose answers agree, the planet stops pulling on it. You cannot be
// dragged into a planet while escaping. collide.c asks WITH the `+ 1` too
// (collide.c:102, 139), so the same fleeing ship also takes no impulse --
// only do_damage (misc.c:214) asks without it, so the ship stays damageable.
//
// Two names, because they are two predicates and conflating them is a bug
// waiting to be reintroduced.
inline constexpr std::int32_t kMaxShipMass = 10;              // element.h:197
inline constexpr std::int32_t kGravityMass = kMaxShipMass * 10;  // 100

// GRAVITY_MASS as written: does this push instead of being pushed?
[[nodiscard]] constexpr bool
isGravityMass(std::int32_t massPoints) noexcept
{
	return massPoints > kGravityMass;
}

// GRAVITY_MASS as gravity.c asks it. See above for why they differ.
[[nodiscard]] constexpr bool
isGravitySource(std::int32_t massPoints) noexcept
{
	return massPoints + 1 > kGravityMass;
}

// What kind of thing this is.
//
// The plan's engine primitive #3: a real tag, not a frame pointer. The C
// identifies element types by comparing FRAME pointers (cyborg.c:1222-1227)
// and by reaching into another ship's header to name its constants --
// shofixti.c:251-253 does `#include "../orz/orz.h"` to recognise an Orz
// turret. Both stop being expressible once the type is a field.
enum class ElementKind : std::uint8_t
{
	Unknown = 0,
	Ship,
	Weapon,
	Asteroid,
	Planet,
	Blast,
	Turret,

	// A beam. Unlike everything else its `current` and `next` are the two
	// *ends* of the thing rather than where it was and where it is going --
	// which is exactly what the C does with a LINE_PRIM (weapon.c:44-85), and
	// why it can reuse the same element for it.
	Laser,

	// A single point of a ship's exhaust, and the shadow a ship leaves while
	// warping in. One element in the C too -- both use cycle_ion_trail
	// (tactrans.c:756-790), which is why they share a kind here.
	IonTrail,

	// A fading silhouette of a ship, shed while it warps in. Ship-shaped, not
	// a point: the C draws the ship's own image as a STAMPFILL_PRIM
	// (tactrans.c:893-930). It shares the ion trail's colour ramp but not its
	// geometry -- and the geometry is what makes the effect visible at all.
	ShipShadow,

	// One spark of a dying ship. The explosion is a *swarm* of these thrown
	// off over 26 frames while the hull is still there (tactrans.c:542-615),
	// not one animation played on the wreck.
	Debris,
};

// Hooks. Free functions taking the battle and the element's own id, so they
// have no captured state and cannot outlive it. The C stores these as
// per-instance function pointers and ships mutate their own -- chmmr.c:773
// deletes its hook and pkunk.c:282 reinstalls one -- which the plan
// deliberately gives up in favour of explicit state machines.
using ElementHook = void (*)(Battle &, EntityId) noexcept;

struct Element
{
	// Where it is now, and where the step is taking it.
	Vec2i current;
	Vec2i next;

	Velocity velocity;

	ElementFlags flags = ElementFlags::None;
	ElementKind kind = ElementKind::Unknown;

	// -1 for things nobody owns, like asteroids.
	std::int32_t playerNr = -1;

	Facing facing;

	// NORMAL_LIFE (element.h:32), not zero. The step loop reads a zero as
	// "died last frame" regardless of FiniteLife, so a persistent element has
	// to start at 1 and simply never decrement.
	std::int32_t lifeSpan = 1;

	std::int32_t hitPoints = 0;
	std::int32_t mass = 0;
	std::int32_t damage = 0;

	// How far along its travel direction a weapon's blast sits, in display
	// pixels, so the explosion lands on the surface it hit rather than inside
	// it (weapon.c:202-208).
	std::int32_t blastOffset = 0;

	// Where an element is in whatever colour or frame sequence it animates
	// through: the ion trail's twelve-step fade, an explosion's frames. The C
	// calls this colorCycleIndex and keeps it on every element for the same
	// reason -- the alternative is a parallel table keyed by entity.
	std::int32_t colorCycle = 0;

	// Frames until the ship may turn or thrust again. A collision adds to
	// both, which is the stagger you feel after hitting something
	// (collide.c:113-116).
	std::int32_t turnWait = 0;
	std::int32_t thrustWait = 0;

	// Not owned: masks live with the content and outlive the battle.
	Borrowed<const CollisionMask> mask = nullptr;

	// The silhouette and facing this element ENTERED the frame with, captured
	// by the step before any hook runs. The overlap-repair protocol
	// (process.c:453-506) uses them to undo a rotation made this frame that
	// turned the element into a wall: the C reverts next.image to
	// current.image and re-reads ShipFacing from it, and these two fields are
	// that current.image for a sim without images.
	Borrowed<const CollisionMask> priorMask = nullptr;
	Facing priorFacing;

	ElementHook preProcess = nullptr;
	ElementHook postProcess = nullptr;
	ElementHook onDeath = nullptr;

	// Runs after a collision has been resolved, on each side, with
	// `collidedWith` already set. This is collision_func in the C.
	ElementHook onCollision = nullptr;

	// What this element hit, valid only inside a collision hook.
	EntityId collidedWith;

	// The ship this came from, and pParent in the C (element.h:192). A ship
	// owns itself. This is what IGNORE_SIMILAR is tested against: the C skips
	// a pair when both carry the flag *and share an owner*, which is what
	// stops a flame burning the Avenger that breathed it. Owner, not player --
	// two ships of the same species on one side still shoot each other.
	EntityId owner;

	// Only meaningful when kind == Ship; `ship.spec` is null otherwise.
	ShipState ship;

	// CollidingElement (collide.h:31-33): NONSOLID *or* DISAPPEARING is out.
	// Something dying this frame must not still be hit, and must not still
	// pull on anything.
	[[nodiscard]] bool
	collidable() const noexcept
	{
		return mask != nullptr
				&& !any(flags
						& (ElementFlags::NonSolid | ElementFlags::Disappearing));
	}
};

}  // namespace uqm::sim

#endif  // UQM2_SIM_ELEMENT_HPP
