// Copyright the Ur-Quan Masters contributors. GPL-2.0-or-later.
//
// Everything the frame needs. A struct rather than globals so the Emscripten
// driver has something to hand back to iterate().

#ifndef UQM2_APP_MELEE_GAME_HPP
#define UQM2_APP_MELEE_GAME_HPP

#include "engine/core/Geometry.hpp"
#include "engine/core/Pacing.hpp"
#include "engine/input/Input.hpp"
#include "game/Camera.hpp"
#include "game/Resources.hpp"
#include "game/SpriteSet.hpp"
#include "platform/Audio.hpp"
#include "platform/Platform.hpp"
#include "sim/Battle.hpp"
#include "sim/Collision.hpp"
#include "sim/Ship.hpp"
#include "sim/World.hpp"

#include <array>
#include <cstdint>
#include <filesystem>
#include <span>
#include <vector>

namespace uqm::melee {

// A solid rectangular collision mask, standing in for a sprite's real one.
[[nodiscard]] sim::CollisionMask block(std::uint32_t w, std::uint32_t h);

// Randomness enters the simulation here or not at all: the sim never reads a
// clock, so a battle is exactly as random as its seed. Printed, because
// determinism is the property and sameness was the bug -- a fixed literal
// here replayed the identical battle every launch, and a seed nobody knows
// cannot be replayed at all.
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

	// Whether each ship's death has been announced, so it is announced once.
	std::array<bool, 2> deathAnnounced{};

	// The starfield, three planes deep (galaxy.c:37-44). Positions are in
	// display pixels on a screen-sized torus, one per plane -- see drawStars.
	std::array<Vec2i, kStarCount> stars{};
	const game::SpriteSet *starArt = nullptr;
	Pacer pacer;
	std::array<input::InputAccumulator, 2> players;

	// Everything content-addressed. Resources owns the sets and the masks
	// inside them, so both outlive the battle -- which is what Element::mask
	// assumes.
	platform::Audio audio;
	game::Resources content;

	// Sound slots, by the index the .snd list gives them. cruiser.snd is
	// primary then secondary; battle.snd is the shared set -- getcrew,
	// shipdies, then the booms.
	std::span<const platform::Sound> cruiserSounds;
	std::span<const platform::Sound> avengerSounds;
	std::span<const platform::Sound> battleSounds;

	const game::SpriteSet *cruiser = nullptr;
	const game::SpriteSet *avenger = nullptr;
	const game::SpriteSet *nuke = nullptr;   // the Cruiser's missile
	const game::SpriteSet *flame = nullptr;  // the Avenger's fire
	const game::SpriteSet *rock = nullptr;
	const game::SpriteSet *world = nullptr;  // the gravity well
	const game::SpriteSet *blast = nullptr;  // a weapon going off
	const game::SpriteSet *boom = nullptr;   // an asteroid coming apart

	// Fallbacks for anything without art yet -- shots, rocks, the planet.
	sim::CollisionMask shipMask = block(12, 12);
	sim::CollisionMask shotMask = block(3, 3);
	sim::CollisionMask rockMask = block(8, 8);
	sim::CollisionMask planetMask = block(28, 28);

	// Per-battle copies of the ship descriptors, so their weapons can be given
	// the collision mask cut from the projectile art. The shared ones in sim/
	// are static and content-free by design; wiring a mask into them would
	// make sim/ depend on what has been loaded.
	sim::ShipSpec cruiserData;
	sim::ShipSpec avengerData;

	std::array<sim::EntityId, 2> ships{};
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

// Everything is at half level for now. The .wav files are mastered loud, and
// with a dozen streams a flame stream alone will clip.
// Halved once already and still too loud, because the real multiplier was
// the number of simultaneous voices rather than the level of each -- see
// platform/Audio.hpp. With one voice per effect this is an honest level.
inline constexpr float kEffectGain = 0.35f;

// battle.snd is getcrew, shipdies, then the four booms.
inline constexpr std::size_t kBoomFirstSlot = 2;

// How large a patch the field tiles over, in display pixels.
//
// The C cannot be copied directly here. galaxy.c:248-259 puts each star at
// `rand % SPACE_WIDTH << factor` with factor = ONE_SHIFT + MAX_REDUCTION
// (BACKGROUND_SHIFT is 3, so that term is zero), which makes plane 0's space
// exactly LOG_SPACE_WIDTH -- the whole arena -- and then draws it at whatever
// reduction the camera is at. So the C's on-screen density *varies with
// zoom*: about three stars at 1:1, and all 180 when zoomed fully out, because
// the view grows to cover the arena while the stars stay put.
//
// This field is deliberately zoom-independent -- that is what removed the
// shimmer -- so it cannot have both ends of that range, and has to pick one
// number. Tiling over the screen gave all 180 at once, which is the fully
// zoomed-out density applied at every zoom, and reads as a nebula. Tiling
// over the arena gave the 1:1 density, which is two or three stars. Four
// screens' worth lands between them at around 45 in view, which is what the
// melee actually looks like at the zooms it spends its time at.
inline constexpr std::int32_t kStarFieldWidth = sim::kSpaceWidth * 2;
inline constexpr std::int32_t kStarFieldHeight = sim::kSpaceHeight * 2;

// Fills in the Game the two-line setUp wrapper cannot do itself: content
// loading (Assets.cpp) followed by battle setup (Game.cpp).
void setUp(Game &g, const std::filesystem::path &content);

// One pass of the outer loop: drain input, run whatever simulation time is
// owed, draw once. Never more than one draw per call, and never a simulation
// step that is not due -- the two rates are independent and only this function
// knows both.
void iterate(Game &g);

}  // namespace uqm::melee

#endif  // UQM2_APP_MELEE_GAME_HPP
