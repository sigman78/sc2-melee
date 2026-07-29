// Copyright the Ur-Quan Masters contributors. GPL-2.0-or-later.

#ifndef UQM2_SIM_SPAWN_HPP
#define UQM2_SIM_SPAWN_HPP

#include "engine/core/Geometry.hpp"
#include "engine/core/Types.hpp"
#include "sim/Trig.hpp"
#include "sim/Velocity.hpp"

#include <array>
#include <span>

namespace uqm::sim {

// Pure spawn descriptors: a spawn fn takes const ShipView, never a
// mutable ship -- the C's AI lookahead leaks a write via a copied
// ELEMENT (umgah.c:330-341; orz.c:249-253 compensates).

// What a spawn function may read. Deliberately small: if a weapon needs
// something not here, that is a conversation about the interface, not a
// reason to reach for a global.
struct ShipView
{
	Vec2i position;          // hotspot, world units
	Velocity velocity;
	Facing facing;
	int playerNr = 0;

	// The ship's own weapon parameters, from its data file.
	i32 weaponSpeed = 0;
	i32 weaponLife = 0;
	i32 weaponDamage = 0;
	i32 weaponHitPoints = 0;
	i32 muzzleOffset = 0;   // pixels forward of the hotspot
	i32 blastOffset = 0;
};

// What comes out. A value, not an object: no handle, no allocation, nothing
// to free on the discard path the AI takes.
struct Spawn
{
	Vec2i position;
	Facing facing;
	i32 speed = 0;
	i32 life = 0;
	i32 damage = 0;
	i32 hitPoints = 0;
	i32 blastOffset = 0;
	int playerNr = 0;

	// Which sprite frame, derived rather than remembered -- this is the field
	// Umgah mutates the ship to set.
	u16 frameIndex = 0;

	// True where the projectile should not collide with others from the same
	// source (IGNORE_SIMILAR).
	bool ignoreSimilar = false;

	// True where the shot rides the ship's own velocity, not left behind. The
	// Ilwrath flame does this (ilwrath.c:219-222), backed off one frame so the
	// stream trails the Avenger; the Cruiser's nuke does not.
	bool inheritsVelocity = false;

	friend bool operator==(const Spawn &, const Spawn &) = default;
};

// The C's Weapon[6] is the ceiling on one shot across every ship.
inline constexpr usize kMaxSpawnsPerShot = 6;
using SpawnBuffer = std::array<Spawn, kMaxSpawnsPerShot>;

// A ship's primary-weapon spawn: const view in, values out, count returned.
// No non-const path to the ship, so the Umgah mutation pattern can't compile.
using SpawnFn = usize (*)(const ShipView &, std::span<Spawn>) noexcept;

// Where a projectile appears: `muzzleOffset` pixels along the facing from the
// hotspot. Pulled out because every ship does it and half of them do it
// slightly differently in the C.
[[nodiscard]] Vec2i muzzlePosition(const ShipView &ship) noexcept;

}  // namespace uqm::sim

#endif  // UQM2_SIM_SPAWN_HPP
