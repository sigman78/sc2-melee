// Copyright the Ur-Quan Masters contributors. GPL-2.0-or-later.

#ifndef UQM2_SIM_SHIP_HPP
#define UQM2_SIM_SHIP_HPP

#include "engine/core/Borrowed.hpp"
#include "engine/core/Types.hpp"
#include "sim/Damage.hpp"
#include "sim/Element.hpp"
#include "sim/Spawn.hpp"
#include "sim/Thrust.hpp"

#include <optional>

#define comp

namespace uqm::sim {

class Battle;

// Free functions taking the battle and the element's id: no captured state,
// so they can't outlive it. The C mutates per-instance hooks at runtime
// (chmmr.c:773 deletes its hook, pkunk.c:282 reinstalls one); this doesn't.
// Spec-level only (review-007 W5): a ship's own preProcess (the Ilwrath
// cloak machine) and a special's activation hook are what is left of
// Element's old ElementHook once every per-element hook dissolved into a
// component or a dispatch keyed on component presence.
using ShipPhase = void (*)(Battle &, EntityId) noexcept;

// What the player is asking for this frame.
enum class ShipInput : u8
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
			static_cast<u8>(a) | static_cast<u8>(b));
}
[[nodiscard]] constexpr ShipInput
operator&(ShipInput a, ShipInput b) noexcept
{
	return static_cast<ShipInput>(
			static_cast<u8>(a) & static_cast<u8>(b));
}
constexpr ShipInput &
operator|=(ShipInput &a, ShipInput b) noexcept
{
	return a = a | b;
}
[[nodiscard]] constexpr bool
any(ShipInput f) noexcept
{
	return static_cast<u8>(f) != 0;
}

// GuidedShot, the census's first library component (review-002 §4): the
// guidance parameters copied from the spec at fire time, plus the shot's
// own tracking clock -- which lived in a repurposed Element::turnWait
// until review-005 Y1. Attached by the fire block to any weapon whose
// spec declares guidance; guidedShotPreProcess is its system function.
// Declared ahead of WeaponSpec (review-007 W9) so a spec can carry one as
// an attach-ready `std::optional<Guided>` literal instead of the three
// scalars a fire block used to repack into one of these on every shot.
comp struct Guided
{
	i32 trackWait = 0;
	i32 maxSpeed = 0;
	i32 thrustScale = 0;
	i32 clock = 0;
};

// A ship's weapon: the primary-fire descriptor plus the shot's own flight
// parameters, guidance, and per-frame/collision hooks.
struct WeaponSpec
{
	i32 wait = 0;
	i32 energyCost = 0;

	// Geometry, handed to the spawn function below (Spawn.hpp's ShipView):
	// needs the ship's own facing to resolve into a Position/Motion, so it
	// stays scalar rather than an attach-ready literal (review-007 W9).
	i32 speed = 0;
	i32 muzzleOffset = 0;

	// Attach-ready component literals (review-007 W9): the fire block
	// attaches these verbatim instead of re-translating scattered scalars
	// into a fresh component on every shot. `lifetime.remaining` and
	// `vitality.hitPoints` are also read as plain scalars elsewhere
	// (guidedShotPreProcess's speed ramp, ShipView's per-shot descriptor) --
	// the spec itself never decrements, only the entity's own copy does.
	Lifetime lifetime;
	Vitality vitality;
	Warhead warhead;

	// Guided-weapon parameters (human.c:36-44), wound and ready to attach
	// verbatim; absent for a weapon that just flies straight, which is most
	// of them. Presence is the query now (review-007 W9), not "any of three
	// scalars nonzero".
	std::optional<Guided> guided;

	// The primary weapon, as a pure descriptor function (Spawn.hpp).
	SpawnFn spawn = nullptr;

	// Stamped onto every shot as FrameDriven (review-007 W5): true only for
	// the flame, whose AnimFrame advances and whose Collider mask follows
	// every frame it lives (ilwrath.c:126-139) -- the animate pass's own
	// sub-iteration, not a per-shot hook. A guided shot's frame follows its
	// facing instead (Human.cpp's guidedShotPreProcess, its own pass).
	bool frameDriven = false;

	// A shot collides as whichever sprite frame it is drawn from, which is
	// not always its facing -- indexing by facing instead is what squashes a
	// missile into the wrong box.
	std::span<const CollisionMask> masks;
};

// A ship's SPECIAL: cost, cooldown and what it does.
struct SpecialSpec
{
	i32 wait = 0;
	i32 energyCost = 0;

	// What SPECIAL does in the post phase; the engine only ticks the
	// counter, everything a special does is per-ship (ship.c:342-346). Null
	// when the special lives in the ship's preProcess hook instead (Avenger).
	ShipPhase hook = nullptr;

	// LASER_RANGE (human.c:55), in display pixels. Zero for a ship without
	// point defence.
	i32 pointDefenceRange = 0;
};

// An immutable description of a ship type: a *value*, constructible in code,
// not only parsed -- sis_ship.c:881-990 builds the flagship's descriptor from
// inventory, shofixti.c:461-517 a damaged variant by copying one.
struct ShipSpec
{
	i32 maxCrew = 0;
	i32 maxEnergy = 0;
	i32 energyRegen = 0;
	i32 energyWait = 0;

	ThrustProfile thrust;
	i32 thrustWait = 0;
	i32 turnWait = 0;

	i32 mass = 0;

	WeaponSpec weapon;
	SpecialSpec special;

	// The ship's own per-frame hook, run inside shipPreProcess after energy
	// regen and before turning -- RACE_DESC.preprocess_func (ship.c:232-236).
	// The Ilwrath cloak engages here, winning the energy race against the shot.
	ShipPhase preProcess = nullptr;

	Borrowed<const CollisionMask> hullMask = nullptr;

	// One mask per facing, in facing order; empty until content is loaded,
	// since sim/ never reads files. A ship's silhouette follows its facing,
	// so collision has to track it rather than whichever cel spawned it.
	std::span<const CollisionMask> facingMasks;

	// Every field defaults to zero, which is not a slow ship but one that
	// cannot accelerate at all, turns every frame and has no crew -- worth
	// being able to tell from a control bug.
	[[nodiscard]] constexpr bool
	valid() const noexcept
	{
		return maxCrew > 0 && thrust.max > 0;
	}
};

// A ship's mutable half. The C keeps this in STARSHIP beside the ELEMENT;
// here it is a registry component keyed by the same entity (review-002 §1,
// review-004 X3), destroyed with it.
comp struct ShipState
{
	// Hooks hold a ShipState& across spawns and other hooks (shipPostProcess'
	// weapon loop); stable addresses are load-bearing, exactly as for Element.
	static constexpr auto in_place_delete = true;

	Borrowed<const ShipSpec> spec = nullptr;

	i32 crew = 0;
	i32 energy = 0;

	i32 energyCounter = 0;
	i32 weaponCounter = 0;
	i32 specialCounter = 0;

	SpeedState speed = SpeedState::Normal;

	// SHIP_IN_GRAVITY_WELL (races.h:71): orthogonal to the at-max/beyond-max
	// pair. Lets a ship accelerate past its own max, up to kMaxAllowedSpeed
	// (ship.c:82,106-112); gravity sets it, next thrust clears it (ship.c:263-267).
	bool inGravityWell = false;

	// Element{turnWait, thrustWait}, split out (review-007 W4b): frames
	// until the ship may turn or thrust again. A collision adds to both,
	// which is the stagger you feel after hitting something
	// (collide.c:113-116, Impulse.cpp). Ship-control clocks, so ShipState is
	// where they belong -- every other Element tenant already left (the
	// asteroid's spin, the GUIDED clock) before this split.
	i32 turnWait = 0;
	i32 thrustWait = 0;
};

// What the player is asking for, as its own component: every ship has one
// (Battle::attachShip), so the app writes it without a ShipState field.
comp struct Input
{
	ShipInput buttons = ShipInput::None;
};

// The weapon-guidance component: which WeaponSpec a shot flies by
// (review-002 §1) -- ends the old abuse of ShipState as a spec-pointer
// carrier on weapons. The pointer is copied out by readers, never held
// into the pool, so default storage suffices.
comp struct WeaponGuidance
{
	Borrowed<const WeaponSpec> spec = nullptr;
};

// The cloak walk: five visible fill colours (levels 1..5), then black.
inline constexpr i32 kCloakVisibleColours = 5;
inline constexpr i32 kCloakFullLevel = kCloakVisibleColours + 1;

// The cloak as its own component (review-004 X5): only a ship that has one
// carries it -- every ShipState used to hold an Ilwrath field, which is
// exactly the state pollution the census's component library exists to end.
// `level` is where the ship is in the colour walk, as an index:
//     0            STAMP -- solid, visible, machine idle
//     1..5         STAMPFILL fills: white, cyan-white, dark cyan, blue,
//                  dark blue (ilwrath.c:349-374 in, 255-273 out)
//     kCloakFullLevel (6)   BLACK -- fully cloaked
// Not a fade: walked one step per frame, reversed to uncloak
// (ships/Ilwrath.cpp).
comp struct Cloak
{
	i32 level = 0;
};

// Presence is the phase; removal replaces the C's per-instance hook swap
// (tactrans.c:868-886, 703-728). shipPreProcess dispatches on these.
comp struct WarpingIn
{
};

comp struct Exploding
{
};

// cleanup_dead_ship (tactrans.c:307-337): attached the instant a ship starts
// exploding (startShipExplosion), replacing the old onDeath =
// sweepDeadShipOrdnance hook -- the death path (Battle.cpp) checks this tag
// instead of a per-element function pointer. A ship's crew, not its hull,
// so nothing else in the census ever carries it (review-007 W5).
comp struct SweepsOwnedOnDeath
{
};

// How long the exhaust fade runs, in frames -- the length of the C's colour
// table (tactrans.c:757-770).
inline constexpr i32 kIonTrailLife = 12;

// HYPERJUMP_LIFE (element.h:69): how long a ship spends warping in, invisible
// and untouchable, before it becomes real.
inline constexpr i32 kWarpInFrames = 15;

// TRANSITION_SPEED (tactrans.c:909): how far apart the images of an arriving
// ship are spaced along its path.
inline constexpr i32 kTransitionSpeed = displayToWorld(40);

// How long one spark of the explosion lasts, and when the hull itself stops
// being drawn -- 15 frames into a 36-frame death (tactrans.c:569-571).
inline constexpr i32 kDebrisLife = 9;
inline constexpr i32 kHullVanishAge = 15;

// NUM_EXPLOSION_FRAMES * 3 (element.h:71, tactrans.c:714).
inline constexpr i32 kExplosionFrames = 12;
inline constexpr i32 kExplosionLife = kExplosionFrames * 3;

}  // namespace uqm::sim

#undef comp

#endif  // UQM2_SIM_SHIP_HPP
