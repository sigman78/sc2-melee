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
#include <cstdint>
#include <filesystem>
#include <vector>

namespace uqm::melee {

// Randomness enters the simulation here or not at all: the sim never reads
// a clock, so a battle is exactly as random as its seed. Printed so a
// battle can be replayed from the logged value.
[[nodiscard]] std::uint32_t battleSeed();

// Total stars across all three parallax planes; see Draw.cpp for the field
// itself.
inline constexpr int kStarCount = 30 + 60 + 90;

struct Game
{
	platform::Platform window{"The Ur-Quan Masters -- melee",
			Extent2u{static_cast<std::uint32_t>(sim::kSpaceWidth),
				static_cast<std::uint32_t>(sim::kSpaceHeight)},
			3};

	sim::Battle battle{battleSeed()};
	game::Camera camera;

	// What each live element draws as, attached at spawn -- see Draw.hpp.
	RenderStore visuals;

	// Whether each ship's death has been announced, so it is announced once.
	std::array<bool, 2> deathAnnounced{};

	// The starfield, three planes deep (galaxy.c:37-44). Positions are in
	// display pixels on a screen-sized torus, one per plane -- see drawStars.
	std::array<Vec2i, kStarCount> stars{};
	Pacer pacer;
	std::array<input::InputAccumulator, 2> players;

	// Everything content-addressed. Resources owns the sets and the masks
	// inside them, so both outlive the battle -- which is what Element::mask
	// assumes.
	platform::Audio audio;
	game::Resources content;

	// Who is fighting, as catalog entries -- parallel to `ships`. Art and
	// sounds resolve through the owner's definition (visualFor, Sound.cpp);
	// no other app code names a resource id.
	std::array<Borrowed<const game::ShipDef>, 2> roster{};

	// Per-battle copies, so their weapons can carry the mask cut from the
	// projectile art. sim/'s shared descriptors stay content-free by
	// design; wiring a mask into them would make sim/ depend on content.
	std::array<sim::ShipSpec, 2> shipData{};

	// kNoEntity explicitly: entt's zero id is a real first entity, not null.
	std::array<sim::EntityId, 2> ships{sim::kNoEntity, sim::kNoEntity};
	bool running = true;

	// -1 while the fight is on, then the surviving player, or 2 for a draw.
	// Held rather than acted on: the battle keeps stepping so the wreck and
	// its blast finish playing out, which is what the C does too.
	int winner = -1;
	std::int64_t endedAtFrame = 0;

	// F1. Off by default; costs nothing when off.
	bool debugOverlay = false;
	// Last step's F1 level, so the toggle fires on the edge. Toggling on the
	// level flipped the overlay 24 times a second while the key was held.
	bool debugWasDown = false;

	// Contact points linger, because a collision lasts one frame and one frame
	// at 24 Hz is not long enough to see. Each entry is an event plus the
	// frame it happened on.
	struct Mark
	{
		sim::CollisionEvent event;
		std::int64_t frame = 0;
	};
	std::vector<Mark> marks;
};

// How long a contact point stays on screen, in simulation frames.
inline constexpr std::int64_t kMarkLife = 24;

// Half level: the .wav files are mastered loud enough that a dozen
// streams would clip. With one voice per effect (platform/Audio.hpp)
// this level is honest, not a blanket fix for voice-count overlap.
inline constexpr float kEffectGain = 0.35f;

// How large a patch the field tiles over, in display pixels. The C
// varies on-screen density with zoom (galaxy.c:248-259); this field is
// zoom-independent, so it tiles over four screens for ~45 stars in view.
inline constexpr std::int32_t kStarFieldWidth = sim::kSpaceWidth * 2;
inline constexpr std::int32_t kStarFieldHeight = sim::kSpaceHeight * 2;

// Fills in the Game the two-line setUp wrapper cannot do itself: content
// loading (Assets.cpp) followed by battle setup (Game.cpp).
void setUp(Game &g, const std::filesystem::path &content);

// One pass of the outer loop: drain input, run whatever simulation time is
// owed, draw once. Never more than one draw per call, and never a step
// that is not due -- the two rates are independent, and only this knows both.
void iterate(Game &g);

}  // namespace uqm::melee

#endif  // UQM2_APP_MELEE_GAME_HPP
