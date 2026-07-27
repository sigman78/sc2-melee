// Copyright the Ur-Quan Masters contributors. GPL-2.0-or-later.

#ifndef UQM2_SIM_SHIP_HPP
#define UQM2_SIM_SHIP_HPP

#include "engine/core/Borrowed.hpp"
#include "engine/core/Types.hpp"
#include "sim/Element.hpp"
#include "sim/Spawn.hpp"
#include "sim/Thrust.hpp"

namespace uqm::sim {

class Battle;

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

// A ship's weapon: the primary-fire descriptor plus the shot's own flight
// parameters, guidance, and per-frame/collision hooks.
struct WeaponSpec
{
	i32 wait = 0;
	i32 energyCost = 0;

	// Handed to the spawn function below (Spawn.hpp's ShipView).
	i32 speed = 0;
	i32 life = 0;
	i32 damage = 0;
	i32 hitPoints = 0;
	i32 muzzleOffset = 0;
	i32 blastOffset = 0;

	// Guided-weapon parameters (human.c:36-44). Zero for a weapon that just
	// flies straight, which is most of them.
	i32 trackWait = 0;
	i32 maxSpeed = 0;
	i32 thrustScale = 0;

	// The primary weapon, as a pure descriptor function (Spawn.hpp).
	SpawnFn spawn = nullptr;

	// What the shot does each frame, if anything.
	ElementHook preProcess = nullptr;

	// What the shot does on impact. Null means weaponCollision (damage,
	// blast, gone); the flame wraps it to linger a frame instead of
	// vanishing (ilwrath.c:141-148).
	ElementHook onCollision = nullptr;

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
	ElementHook hook = nullptr;

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
	ElementHook preProcess = nullptr;

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
struct ShipState
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

};

// What the player is asking for, as its own component: every ship has one
// (Battle::attachShip), so the app writes it without a ShipState field.
struct Input
{
	ShipInput buttons = ShipInput::None;
};

// The weapon-guidance component: which WeaponSpec a shot flies by
// (review-002 §1) -- ends the old abuse of ShipState as a spec-pointer
// carrier on weapons. The pointer is copied out by readers, never held
// into the pool, so default storage suffices.
struct WeaponGuidance
{
	Borrowed<const WeaponSpec> spec = nullptr;
};

// GuidedShot, the census's first library component (review-002 §4): the
// guidance parameters copied from the spec at fire time, plus the shot's
// own tracking clock -- which lived in a repurposed Element::turnWait
// until review-005 Y1. Attached by the fire block to any weapon whose
// spec declares guidance; nukePreProcess is its system function.
struct Guided
{
	i32 trackWait = 0;
	i32 maxSpeed = 0;
	i32 thrustScale = 0;
	i32 clock = 0;
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
// Not a fade: walked one step per frame, reversed to uncloak (Ship.cpp).
struct Cloak
{
	i32 level = 0;
};

// Presence is the phase; removal replaces the C's per-instance hook swap
// (tactrans.c:868-886, 703-728). shipPreProcess dispatches on these.
struct WarpingIn
{
};

struct Exploding
{
};

// OBJECT_CLOAKED, derived: hidden from weapon targeting (weapon.c:344) and
// PD (human.c:202) only at full black (element.h:201-204). Computed from
// the component, never stored beside it -- the flag this replaces violated
// review-002's stored-vs-derived rule by construction.
[[nodiscard]] bool isCloaked(const Battle &b, EntityId id) noexcept;

// The two halves of a ship's frame (ship.c:149-280, 282-347): turning and
// thrusting in the pre pass, firing in the post pass so a spawned weapon is
// caught up by the step loop this frame -- see design-notes D1.
void shipPreProcess(Battle &b, EntityId id) noexcept;
void shipPostProcess(Battle &b, EntityId id) noexcept;

// TrackShip (weapon.c:319-412): steers `facing` toward the nearest living
// enemy ship (by player, not owner). Returns -1 for no target, else the
// facing delta -- load-bearing: cloak auto-aim tests `>= 0`, nuke ignores it.
[[nodiscard]] int trackShip(Battle &b, EntityId tracker, Facing &facing,
		EntityId *outTarget = nullptr) noexcept;

// The Cruiser's nuke, which is guided and accelerates as it flies
// (human.c:128-158).
void nukePreProcess(Battle &b, EntityId id) noexcept;

// The Cruiser's point-defence laser (human.c:161-260): burns down every enemy
// shot in range, paying once for the volley.
void cruiserSpecial(Battle &b, EntityId id) noexcept;

// The Avenger's ship hook: the whole cloak state machine, activation
// included (ilwrath_preprocess, ilwrath.c:232-394). Runs in the pre phase
// because the C's does -- see ShipSpec::preProcess.
void ilwrathPreProcess(Battle &b, EntityId id) noexcept;

// The Avenger's flame: the animation is the projectile -- its frame (and
// collision silhouette) grows every frame it lives (ilwrath.c:126-139), and
// lingers one frame on impact instead of vanishing (ilwrath.c:141-148).
void flamePreProcess(Battle &b, EntityId id) noexcept;
void flameCollision(Battle &b, EntityId id) noexcept;

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

// One point of exhaust, dropped behind a thrusting ship (tactrans.c:792-840).
void spawnIonTrail(Battle &b, EntityId ship) noexcept;

// Turns a dead ship into its own explosion rather than removing it
// (StartShipExplosion, tactrans.c:703-728).
void startShipExplosion(Battle &b, EntityId id) noexcept;

const ShipSpec &earthlingCruiser() noexcept;
[[nodiscard]] const ShipSpec &ilwrathAvenger() noexcept;

}  // namespace uqm::sim

#endif  // UQM2_SIM_SHIP_HPP
