// Copyright the Ur-Quan Masters contributors. GPL-2.0-or-later.

#ifndef UQM2_SIM_SHIP_HPP
#define UQM2_SIM_SHIP_HPP

#include "engine/core/Borrowed.hpp"
#include "sim/Element.hpp"
#include "sim/Spawn.hpp"
#include "sim/Thrust.hpp"

#include <cstdint>

namespace uqm::sim {

class Battle;

// A ship's weapon: the primary-fire descriptor plus the shot's own flight
// parameters, guidance, and per-frame/collision hooks.
struct WeaponSpec
{
	std::int32_t wait = 0;
	std::int32_t energyCost = 0;

	// Handed to the spawn function below (Spawn.hpp's ShipView).
	std::int32_t speed = 0;
	std::int32_t life = 0;
	std::int32_t damage = 0;
	std::int32_t hitPoints = 0;
	std::int32_t muzzleOffset = 0;
	std::int32_t blastOffset = 0;

	// Guided-weapon parameters (human.c:36-44). Zero for a weapon that just
	// flies straight, which is most of them.
	std::int32_t trackWait = 0;
	std::int32_t maxSpeed = 0;
	std::int32_t thrustScale = 0;

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
	std::int32_t wait = 0;
	std::int32_t energyCost = 0;

	// What SPECIAL does in the post phase; the engine only ticks the
	// counter, everything a special does is per-ship (ship.c:342-346). Null
	// when the special lives in the ship's preProcess hook instead (Avenger).
	ElementHook hook = nullptr;

	// LASER_RANGE (human.c:55), in display pixels. Zero for a ship without
	// point defence.
	std::int32_t pointDefenceRange = 0;
};

// An immutable description of a ship type: a *value*, constructible in code,
// not only parsed -- sis_ship.c:881-990 builds the flagship's descriptor from
// inventory, shofixti.c:461-517 a damaged variant by copying one.
struct ShipSpec
{
	std::int32_t maxCrew = 0;
	std::int32_t maxEnergy = 0;
	std::int32_t energyRegen = 0;
	std::int32_t energyWait = 0;

	ThrustProfile thrust;
	std::int32_t thrustWait = 0;
	std::int32_t turnWait = 0;

	std::int32_t mass = 0;

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
inline constexpr std::int32_t kIonTrailLife = 12;

// HYPERJUMP_LIFE (element.h:69): how long a ship spends warping in, invisible
// and untouchable, before it becomes real.
inline constexpr std::int32_t kWarpInFrames = 15;

// TRANSITION_SPEED (tactrans.c:909): how far apart the images of an arriving
// ship are spaced along its path.
inline constexpr std::int32_t kTransitionSpeed = displayToWorld(40);

// How long one spark of the explosion lasts, and when the hull itself stops
// being drawn -- 15 frames into a 36-frame death (tactrans.c:569-571).
inline constexpr std::int32_t kDebrisLife = 9;
inline constexpr std::int32_t kHullVanishAge = 15;

// NUM_EXPLOSION_FRAMES * 3 (element.h:71, tactrans.c:714).
inline constexpr std::int32_t kExplosionFrames = 12;
inline constexpr std::int32_t kExplosionLife = kExplosionFrames * 3;

// One point of exhaust, dropped behind a thrusting ship (tactrans.c:792-840).
void spawnIonTrail(Battle &b, EntityId ship) noexcept;

// The warp-in preprocess. A ship starts under this hook and swaps itself over
// to shipPreProcess once it has arrived (ship_transition, tactrans.c:852-890).
void shipTransition(Battle &b, EntityId id) noexcept;

// Turns a dead ship into its own explosion rather than removing it
// (StartShipExplosion, tactrans.c:703-728).
void startShipExplosion(Element &e) noexcept;

// Throws off sparks while a dying ship burns. tactrans.c:542-615.
void explosionPreProcess(Battle &b, EntityId id) noexcept;

const ShipSpec &earthlingCruiser() noexcept;
[[nodiscard]] const ShipSpec &ilwrathAvenger() noexcept;

}  // namespace uqm::sim

#endif  // UQM2_SIM_SHIP_HPP
