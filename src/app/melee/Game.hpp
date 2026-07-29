// Copyright the Ur-Quan Masters contributors. GPL-2.0-or-later.
//
// Everything the frame needs. A struct rather than globals so the Emscripten
// driver has something to hand back to iterate().

#ifndef UQM2_APP_MELEE_GAME_HPP
#define UQM2_APP_MELEE_GAME_HPP

#include "app/melee/Draw.hpp"
#include "engine/core/Borrowed.hpp"
#include "engine/core/Geometry.hpp"
#include "engine/core/Pacing.hpp"
#include "engine/core/Types.hpp"
#include "engine/input/Input.hpp"
#include "game/Camera.hpp"
#include "game/Resources.hpp"
#include "game/Ships.hpp"
#include "platform/Audio.hpp"
#include "platform/Platform.hpp"
#include "sim/Battle.hpp"
#include "sim/Collision.hpp"
#include "sim/Ship.hpp"
#include "sim/World.hpp"

#include <array>
#include <filesystem>
#include <vector>

namespace uqm::melee {

// Randomness enters the simulation here or not at all: the sim never reads
// a clock, so a battle is exactly as random as its seed. Printed so a
// battle can be replayed from the logged value.
[[nodiscard]] u32 battleSeed();

struct Game
{
	platform::Platform window{"The Ur-Quan Masters -- melee",
			Extent2u{static_cast<u32>(sim::kSpace.w),
					static_cast<u32>(sim::kSpace.h)},
			3};

	sim::Battle battle{battleSeed()};
	Pacer pacer;
	std::array<input::InputAccumulator, 2> players;

	// Everything content-addressed. Resources owns the sets and the masks
	// inside them, so both outlive the battle -- which is what Collider
	// assumes.
	platform::Audio audio;
	game::Resources content;

	bool running = true;
};

// World-scoped state that is no entity's, reached via Battle::setContext<T>/
// context<T> rather than a Game member. Device handles and frame plumbing
// stay in Game -- ctx for those would be a service locator with extra steps.

// -1 while the fight is on, then the surviving player, or 2 for a draw.
// Held rather than acted on: the battle keeps stepping so the wreck and its
// blast finish playing out, which is what the C does too.
struct MatchState
{
	i32 winner = -1;
	i64 endedAtFrame = 0;

	// kNoEntity explicitly: entt's zero id is a real first entity, not null.
	std::array<sim::EntityId, 2> shipIds{sim::kNoEntity, sim::kNoEntity};
};

struct DebugToggles
{
	// F1. Off by default; costs nothing when off.
	bool overlay = false;

	// Last step's F1 level, so the toggle fires on the edge. Toggling on the
	// level flipped the overlay 24 times a second while the key was held.
	bool wasDown = false;
};

// Who is fighting: catalog entries parallel to `shipIds`, plus per-battle
// materialized specs. Art/sounds resolve through the owner's definition
// (visualFor, Sound.cpp) -- no other app code names a resource id.
struct BattleConfig
{
	std::array<Borrowed<const game::ShipDef>, 2> roster{};

	// Per-battle copies, so their weapons can carry the mask cut from the
	// projectile art. sim/'s shared descriptors stay content-free by
	// design; wiring a mask into them would make sim/ depend on content.
	std::array<sim::ShipSpec, 2> shipData{};
};

// Half level: the .wav files are mastered loud enough that a dozen
// streams would clip. With one voice per effect (platform/Audio.hpp)
// this level is honest, not a blanket fix for voice-count overlap.
inline constexpr float kEffectGain = 0.35f;

// Fills in the Game the two-line setUp wrapper cannot do itself: content
// loading (Assets.cpp) followed by battle setup (Game.cpp).
void setUp(Game &g, const std::filesystem::path &content);

// One pass of the outer loop: drain input, run whatever simulation time is
// owed, draw once. Never more than one draw per call, and never a step
// that is not due -- the two rates are independent, and only this knows both.
void iterate(Game &g);

}  // namespace uqm::melee

#endif  // UQM2_APP_MELEE_GAME_HPP
