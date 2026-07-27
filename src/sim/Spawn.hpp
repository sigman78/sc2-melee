// Copyright the Ur-Quan Masters contributors. GPL-2.0-or-later.

#ifndef UQM2_SIM_SPAWN_HPP
#define UQM2_SIM_SPAWN_HPP

#include "engine/core/Geometry.hpp"
#include "sim/Element.hpp"
#include "sim/Trig.hpp"
#include "sim/Velocity.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

namespace uqm::sim {

// Pure spawn descriptors: engine primitive #5. AI lookahead used to mutate
// real ship state via a copied ELEMENT (umgah.c:330-341 leaks a write;
// orz.c:249-253 compensates); a spawn fn takes const ShipView, no write path.

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
	std::int32_t weaponSpeed = 0;
	std::int32_t weaponLife = 0;
	std::int32_t weaponDamage = 0;
	std::int32_t weaponHitPoints = 0;
	std::int32_t muzzleOffset = 0;   // pixels forward of the hotspot
	std::int32_t blastOffset = 0;
};

// What comes out. A value, not an object: no handle, no allocation, nothing
// to free on the discard path the AI takes.
struct Spawn
{
	Vec2i position;
	Facing facing;
	std::int32_t speed = 0;
	std::int32_t life = 0;
	std::int32_t damage = 0;
	std::int32_t hitPoints = 0;
	std::int32_t blastOffset = 0;
	int playerNr = 0;

	// Which sprite frame, derived rather than remembered -- this is the field
	// Umgah mutates the ship to set.
	std::uint16_t frameIndex = 0;

	// True where the projectile should not collide with others from the same
	// source (IGNORE_SIMILAR).
	bool ignoreSimilar = false;

	// True where the shot rides the ship's own velocity, not left behind. The
	// Ilwrath flame does this (ilwrath.c:219-222), backed off one frame so the
	// stream trails the Avenger; the Cruiser's nuke does not.
	bool inheritsVelocity = false;

	// Per-frame behaviour, if any: a function pointer keeps the descriptor a
	// pure value, safe to produce a hundred times for a lookahead and discard,
	// while still letting a guided missile be guided. Null if the shot just flies.
	ElementHook preProcess = nullptr;

	friend bool operator==(const Spawn &, const Spawn &) = default;
};

// The C's Weapon[6] is the ceiling on one shot across every ship.
inline constexpr std::size_t kMaxSpawnsPerShot = 6;
using SpawnBuffer = std::array<Spawn, kMaxSpawnsPerShot>;

// A ship's primary-weapon spawn: const view in, values out, count returned.
// No non-const path to the ship, so the Umgah mutation pattern can't compile.
using SpawnFn = std::size_t (*)(const ShipView &, std::span<Spawn>) noexcept;

// Where a projectile appears: `muzzleOffset` pixels along the facing from the
// hotspot. Pulled out because every ship does it and half of them do it
// slightly differently in the C.
[[nodiscard]] Vec2i muzzlePosition(const ShipView &ship) noexcept;

// --------------------------------------------------------------------------
// The two M1 weapons, as pure functions.

// Earthling Cruiser: one forward missile.
[[nodiscard]] std::size_t spawnCruiserPrimary(
		const ShipView &ship, std::span<Spawn> out) noexcept;

// Ilwrath Avenger: one forward flame.
[[nodiscard]] std::size_t spawnAvengerPrimary(
		const ShipView &ship, std::span<Spawn> out) noexcept;

}  // namespace uqm::sim

#endif  // UQM2_SIM_SPAWN_HPP
