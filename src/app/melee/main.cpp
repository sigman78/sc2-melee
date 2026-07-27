// Copyright the Ur-Quan Masters contributors. GPL-2.0-or-later.
//
// The M1 vertical slice: Human Cruiser against Ilwrath Avenger, two players at
// one keyboard, on a real window.
//
// Ships, projectiles, asteroids and the planet all draw from content. Anything
// without art yet -- blasts, mainly -- falls back to a coloured rectangle,
// which is deliberately ugly so it reads as missing rather than as a choice.
//
// Everything is positioned by its cel's *hotspot*, not its centre. Each facing
// has its own: the Cruiser's sixteen are at (7,19), (12,19), (16,15) and so
// on, because the ship is not symmetric and each rendering sits differently in
// its box. Centring instead makes the hull wander as it turns, and puts the
// sprite off the collision mask, which is anchored to that same hotspot.
//
// The loop is the plan's `main` + `iterate()`: one function that advances
// everything by whatever time has passed, called from a driver that differs
// per platform. Emscripten cannot own the outer while-loop -- it has to return
// to the browser between frames -- and writing that shape from the start is
// what stops the desktop build growing a structure the web build cannot use.

#include "engine/core/Pacing.hpp"
#include "engine/input/Input.hpp"
#include "game/Camera.hpp"
#include "game/Resources.hpp"
#include "platform/Audio.hpp"
#include "game/SpriteSet.hpp"
#include "platform/Platform.hpp"
#include "sim/Battle.hpp"
#include "sim/Damage.hpp"
#include "sim/Field.hpp"
#include "sim/Ship.hpp"
#include "sim/World.hpp"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <span>
#include <cstdint>
#include <utility>
#include <vector>

#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#endif

namespace {

using namespace uqm;
using uqm::input::Button;
using uqm::input::InputAccumulator;

// A solid rectangular collision mask, standing in for a sprite's real one.
sim::CollisionMask
block(std::uint32_t w, std::uint32_t h)
{
	const std::vector<std::uint8_t> bits(static_cast<std::size_t>(w) * h, 1);
	return sim::CollisionMask(Extent2u{w, h},
			Vec2i{static_cast<std::int32_t>(w / 2),
				static_cast<std::int32_t>(h / 2)},
			bits);
}

// What the accumulator reports, in the simulation's vocabulary.
//
// This mapping lives here rather than in sim/ or engine/ because it is the
// only place that legitimately knows both. Left beats right, which is what
// battle.c:201-204 does with its `else if` -- pressing both is not a way to
// turn twice as fast, or to turn not at all.
[[nodiscard]] sim::ShipInput
toShipInput(input::Buttons b) noexcept
{
	sim::ShipInput in = sim::ShipInput::None;
	if (b.test(Button::Left))
		in |= sim::ShipInput::Left;
	else if (b.test(Button::Right))
		in |= sim::ShipInput::Right;
	if (b.test(Button::Thrust))
		in |= sim::ShipInput::Thrust;
	if (b.test(Button::Weapon))
		in |= sim::ShipInput::Weapon;
	if (b.test(Button::Special))
		in |= sim::ShipInput::Special;
	return in;
}

struct Colour
{
	std::uint8_t r, g, b;
};

[[nodiscard]] Colour
colourFor(const sim::Element &e) noexcept
{
	switch (e.kind)
	{
		case sim::ElementKind::Ship:
			return e.playerNr == 0 ? Colour{0x40, 0xC0, 0xFF}
								   : Colour{0xFF, 0x60, 0x40};
		case sim::ElementKind::Weapon:
			return Colour{0xFF, 0xE0, 0x60};
		case sim::ElementKind::Asteroid:
			return Colour{0x90, 0x88, 0x78};
		case sim::ElementKind::Planet:
			return Colour{0x60, 0x90, 0x50};
		case sim::ElementKind::Blast:
			return Colour{0xFF, 0xFF, 0xC0};
		default:
			return Colour{0xC0, 0xC0, 0xC0};
	}
}

// A 3x5 digit font, one bit per pixel, packed a row per nibble.
//
// Deliberately not the game's own font. That path exists -- FontDir parses
// .fon and the browser renders them -- but wiring it in means glyph atlases,
// kerning and a text layer, and this is a readout of two numbers per player.
// The real status panel is M2 work and will use the real fonts; this is
// scaffolding that says what the simulation thinks is true.
constexpr std::array<std::uint16_t, 10> kDigits{{
		0b111'101'101'101'111,  // 0
		0b010'110'010'010'111,  // 1
		0b111'001'111'100'111,  // 2
		0b111'001'111'001'111,  // 3
		0b101'101'111'001'001,  // 4
		0b111'100'111'001'111,  // 5
		0b111'100'111'101'111,  // 6
		0b111'001'001'001'001,  // 7
		0b111'101'111'101'111,  // 8
		0b111'101'111'001'111,  // 9
}};

void
drawDigit(platform::Platform &w, int digit, Vec2i at, int scale, Colour c)
{
	if (digit < 0 || digit > 9)
		return;
	const std::uint16_t bits = kDigits[static_cast<std::size_t>(digit)];
	for (int row = 0; row < 5; ++row)
	{
		for (int col = 0; col < 3; ++col)
		{
			// Most significant bit is the top-left pixel.
			const int bit = 14 - (row * 3 + col);
			if (((bits >> bit) & 1) == 0)
				continue;
			w.fillRect(Vec2i{at.x + col * scale, at.y + row * scale},
					Extent2u{static_cast<std::uint32_t>(scale),
						static_cast<std::uint32_t>(scale)},
					c.r, c.g, c.b);
		}
	}
}

// Right-aligned, so a number shrinking from 18 to 9 does not shift about.
void
drawNumber(platform::Platform &w, std::int32_t value, Vec2i rightTop,
		int scale, Colour c)
{
	if (value < 0)
		value = 0;
	int x = rightTop.x - 3 * scale;
	do
	{
		drawDigit(w, static_cast<int>(value % 10), Vec2i{x, rightTop.y}, scale,
				c);
		value /= 10;
		x -= 4 * scale;
	} while (value != 0);
}

// Everything the frame needs. A struct rather than globals so the Emscripten
// driver has something to hand back to iterate().
// The starfield.
//
// Three planes, 30 big, 60 medium and 90 small (galaxy.c:37-44). The C stores
// each plane in a coordinate space 2^plane larger than the arena and then
// shifts all three by the *same* delta each frame (galaxy.c:405-407), so a
// plane in a larger space slides proportionally less across the screen. That
// is where the parallax comes from, and it is the only thing about the C's
// implementation worth keeping: the rest of galaxy.c is an incremental
// scroll over a y-sorted array, wrapping stars one at a time as they fall off
// an edge, which exists to avoid recomputing 180 positions on a 386 and buys
// nothing here.
//
// Stated directly instead: plane p scrolls at 1/2^p of the camera. Plane 0 is
// nearest -- biggest, brightest, fastest -- and plane 2 is the far haze.
inline constexpr int kStarPlanes = 3;
inline constexpr std::array<int, kStarPlanes> kStarsPerPlane{{30, 60, 90}};
inline constexpr int kStarCount = 30 + 60 + 90;

// Only a fallback, for when the art is missing. The cels carry their own
// colour and their own brightness, and they are already graded by plane --
// which is the whole reason they must be drawn as *frames* and not as
// silhouettes.
//
// Drawing the silhouette was the defect: a silhouette is a flat fill of every
// non-transparent pixel, and these glyphs are mostly near-black. The 11x11
// cel is a bright core with almost-invisible diffraction arms, and the 5x5 is
// a single bright pixel inside a faint halo. Filling them turned three subtle
// stars into solid blobs and made the sky unreadable.
inline constexpr std::array<Colour, kStarPlanes> kStarColours{{
		Colour{0x94, 0x9C, 0xFC},
		Colour{0x80, 0x8C, 0xFC},
		Colour{0xA4, 0xAC, 0xFC},
}};

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

// Which cel each plane draws: 11x11 near, 5x5 mid, a single pixel far.
// star_frame_ofs (galaxy.c:316) picks the largest group for the nearest plane
// and the smallest for the farthest, which is the same ordering.
inline constexpr std::array<std::size_t, kStarPlanes> kStarCels{{1, 0, 2}};

struct Game
{
	platform::Platform window{"The Ur-Quan Masters -- melee",
			Extent2u{static_cast<std::uint32_t>(sim::kSpaceWidth),
				static_cast<std::uint32_t>(sim::kSpaceHeight)},
			3};

	sim::Battle battle{0x2A5B};
	game::Camera camera;

	// The starfield, three planes deep (galaxy.c:37-44). Positions are in
	// display pixels on a screen-sized torus, one per plane -- see drawStars.
	std::array<Vec2i, kStarCount> stars{};
	const game::SpriteSet *starArt = nullptr;
	Pacer pacer;
	std::array<InputAccumulator, 2> players;

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
	sim::ShipData cruiserData;
	sim::ShipData avengerData;

	std::array<sim::EntityId, 2> ships{};
	bool running = true;

	// -1 while the fight is on, then the surviving player, or 2 for a draw.
	// Held rather than acted on: the battle keeps stepping so the wreck and
	// its blast finish playing out, which is what the C does too.
	int winner = -1;
	std::int64_t endedAtFrame = 0;

	// F1. Off by default; costs nothing when off.
	bool debugOverlay = false;

	// Contact points linger, because a collision lasts one frame and one frame
	// at 24 Hz is not long enough to see. Each entry is an event plus the
	// frame it happened on.
	struct Mark
	{
		sim::CollisionEvent event;
		std::int64_t frame = 0;
	};
	std::vector<Mark> marks;

	// How many shots were in flight last step, so a new one can be heard.
	std::size_t lastShots = 0;
};

// How long a contact point stays on screen, in simulation frames.
constexpr std::int64_t kMarkLife = 24;

// Everything is at half level for now. The .wav files are mastered loud, and
// with a dozen streams a flame stream alone will clip.
// Halved once already and still too loud, because the real multiplier was
// the number of simultaneous voices rather than the level of each -- see
// platform/Audio.hpp. With one voice per effect this is an honest level.
constexpr float kEffectGain = 0.35f;

// battle.snd is getcrew, shipdies, then the four booms.
constexpr std::size_t kBoomFirstSlot = 2;

// The Ilwrath cloak ramp, converted from the C's 5-bit RGB
// (ilwrath.c:255-275). Index 0 is unused -- level 0 means "draw the real
// sprite" -- and the last step is invisible, so it is not listed.
// cycle_ion_trail's colour table (tactrans.c:757-770), 5-bit RGB widened to
// 8. Yellow-white at the muzzle through red to almost-black, one step a frame.
constexpr std::array<Colour, 12> kIonRamp{{
		Colour{0xFF, 0xAD, 0x00},
		Colour{0xFF, 0x8C, 0x00},
		Colour{0xFF, 0x73, 0x00},
		Colour{0xFF, 0x52, 0x00},
		Colour{0xFF, 0x39, 0x00},
		Colour{0xFF, 0x18, 0x00},
		Colour{0xFF, 0x00, 0x00},
		Colour{0xDE, 0x00, 0x00},
		Colour{0xBD, 0x00, 0x00},
		Colour{0x9C, 0x00, 0x00},
		Colour{0x7B, 0x00, 0x00},
		Colour{0x5A, 0x00, 0x00},
}};

constexpr std::array<Colour, 6> kCloakRamp{{
		Colour{0xFF, 0xFF, 0xFF},  // unused; level 0 draws the sprite
		Colour{0xFF, 0xFF, 0xFF},  // 1F,1F,1F
		Colour{0x52, 0xFF, 0xFF},  // 0A,1F,1F
		Colour{0x00, 0xA5, 0xA5},  // 00,14,14
		Colour{0x52, 0x52, 0xFF},  // 0A,0A,1F
		Colour{0x00, 0x00, 0xA5},  // 00,00,14
}};

// Which sprite set an element draws from. Ownership decides the weapon art,
// because a missile belongs to whoever fired it.
const game::SpriteSet *
spritesFor(const Game &g, const sim::Element &e) noexcept
{
	const game::SpriteSet *set = nullptr;
	switch (e.kind)
	{
		case sim::ElementKind::Ship:
		case sim::ElementKind::ShipShadow:
			// A warp-in shadow is the ship's own image, so it borrows the art.
			set = e.playerNr == 0 ? g.cruiser : g.avenger;
			break;
		case sim::ElementKind::Weapon:
			set = e.playerNr == 0 ? g.nuke : g.flame;
			break;
		case sim::ElementKind::Asteroid:
			set = g.rock;
			break;
		case sim::ElementKind::Planet:
			set = g.world;
			break;
		case sim::ElementKind::Laser:
			return nullptr;  // drawn as a line, not a sprite
		case sim::ElementKind::Blast:
			// Weapon blasts and asteroid debris share a kind but not art. The
			// rubble an asteroid leaves is unowned; a blast belongs to the
			// shot that made it.
			set = e.playerNr < 0 ? g.boom : g.blast;
			break;
		default:
			return nullptr;
	}
	return set != nullptr && set->valid() ? set : nullptr;
}

// Where the content tree is.
//
// The working directory is not the answer. Launched from Explorer it is
// wherever the shell felt like; launched from a build tree it is the build
// tree. So look in both the working directory and beside the executable, and
// walk upward from each -- which is what makes running straight out of
// build/release/src work without arguments.
//
// A directory only counts if it has uqm.rmp in it. Finding a `sc2/content`
// that is empty and then drawing rectangles is exactly the failure this is
// meant to stop.
[[nodiscard]] std::filesystem::path
findContent(const std::filesystem::path &override_)
{
	namespace fs = std::filesystem;

	const auto looksRight = [](const fs::path &p) {
		std::error_code ec;
		return fs::exists(p / "uqm.rmp", ec);
	};

	if (!override_.empty())
		return override_;  // the user said so; do not second-guess it

	std::error_code ec;
	for (fs::path start : {fs::current_path(ec), platform::executableDirectory()})
	{
		for (int up = 0; up < 6 && !start.empty(); ++up)
		{
			if (looksRight(start / "sc2" / "content"))
				return start / "sc2" / "content";
			if (looksRight(start / "content"))
				return start / "content";
			if (!start.has_parent_path() || start.parent_path() == start)
				break;
			start = start.parent_path();
		}
	}
	return {};
}

void
setUp(Game &g, const std::filesystem::path &content)
{
	if (content.empty())
	{
		std::fprintf(stderr,
				"content: not found.\n"
				"  Looked for sc2/content/uqm.rmp beside the executable and in\n"
				"  the working directory, and upward from both.\n"
				"  Pass it explicitly:  uqm2-melee <path-to>/sc2/content\n"
				"  Continuing without art -- everything will be a rectangle.\n");
	}
	else
	{
		// stderr, not stdout: this is diagnostic, and stdout is block-buffered
		// when redirected, so a printf here is lost if the process is killed
		// rather than exited -- which is exactly how you run a game.
		std::fprintf(stderr, "content: %s\n", content.string().c_str());
	}

	g.content = game::Resources::open(content);
	if (!g.content.valid())
	{
		std::fprintf(stderr,
				"content: %s has no readable uqm.rmp -- everything will be a "
				"rectangle.\n",
				content.string().c_str());
	}

	// Addressed by resource id, not by path. uqm.rmp is the only link between
	// a name and a file, and it is what an addon overrides -- see
	// game/Resources.hpp.
	const auto load = [&](const char *id) -> const game::SpriteSet * {
		const game::SpriteSet &set = g.content.sprites(g.window, id);
		if (!set.valid() && g.content.valid())
			std::fprintf(stderr, "content: could not load %s\n", id);
		return &set;
	};

	g.cruiser = load("ship.earthling.graphics.human.large");
	g.avenger = load("ship.ilwrath.graphics.avenger.large");
	g.nuke = load("ship.earthling.graphics.saturn.large");
	g.flame = load("ship.ilwrath.graphics.fire.large");
	g.rock = load("graphics.asteroid.large");
	g.blast = load("graphics.blast.large");
	g.boom = load("graphics.boom.large");
	// The C picks a planet type at random per battle (load_gravity_well,
	// cons_res.c:52-82). One fixed type until melee setup exists to choose.
	g.world = load("planet.acid.large");
	g.starArt = load("graphics.stars");

	const auto loadSounds = [&](const char *id) {
		const std::span<const platform::Sound> set =
				g.content.sounds(g.audio, id);
		std::size_t ok = 0;
		for (const platform::Sound &snd : set)
			ok += snd.valid() ? 1 : 0;
		std::fprintf(stderr, "audio: %s -> %zu/%zu loaded\n", id, ok,
				set.size());
		return set;
	};
	g.cruiserSounds = loadSounds("ship.earthling.sounds");
	g.avengerSounds = loadSounds("ship.ilwrath.sounds");
	g.battleSounds = loadSounds("sounds.battle");
	if (!g.audio.valid())
		std::fprintf(stderr, "audio: no device; the game runs silent\n");

	// The descriptors first, then the content-derived masks on top. Losing
	// these two lines leaves a default-constructed ShipData, whose maxThrust
	// is 0 and whose turnWait is 0 -- a ship that cannot accelerate and spins
	// every frame, with no crew and no weapon. It looks like a control bug and
	// is not.
	g.cruiserData = sim::earthlingCruiser();
	g.avengerData = sim::ilwrathAvenger();

	g.cruiserData.facingMasks = g.cruiser->masks;
	g.avengerData.facingMasks = g.avenger->masks;
	g.cruiserData.weaponMasks = g.nuke->masks;
	g.avengerData.weaponMasks = g.flame->masks;

	if (!g.cruiserData.valid() || !g.avengerData.valid())
	{
		std::fprintf(stderr,
				"ship: a descriptor was never filled in -- the ships will not "
				"fly. This is a setup bug, not a control one.\n");
	}

	const auto addShip = [&g](const sim::ShipData &data, Vec2i at, int facing,
							  int player) {
		sim::Element e;
		e.kind = sim::ElementKind::Ship;
		e.flags = sim::ElementFlags::PlayerShip | sim::ElementFlags::IgnoreSimilar;
		e.current = at;
		e.next = at;
		e.facing = facing;
		e.playerNr = player;
		e.mass = data.mass;

		// The real silhouette when the art loaded, a block when it did not.
		// Per-pixel collision against a 12x12 square is not per-pixel
		// collision, so this changes how the ships actually touch.
		const game::SpriteSet *set = player == 0 ? g.cruiser : g.avenger;
		const sim::CollisionMask *m =
				set != nullptr ? set->maskFor(facing) : nullptr;
		e.mask = m != nullptr ? m : &g.shipMask;
		e.ship.data = &data;
		// Warping in, not simply present. shipTransition hands over to
		// shipPreProcess once the ship has arrived.
		e.preProcess = sim::shipTransition;
		e.postProcess = nullptr;
		e.onCollision = sim::solidCollision;
		return g.battle.spawnBack(std::move(e));
	};

	// Random facings and random positions, as the C does (ship.c:456, 473).
	// The facing has to be chosen before the ship is spawned, because the
	// collision mask is per-facing and placement tests that mask.
	const auto randomFacing = [&g] {
		return sim::normalizeFacing(
				static_cast<int>(g.battle.rng().next() & 0xFF));
	};

	// Two ships dropped anywhere can land next to each other, and a melee that
	// opens with the ships already touching is not a melee. 1024 world units
	// is what the fixed opening used to be -- far enough to fly at each other,
	// close enough that the camera holds both.
	constexpr std::int32_t kMinSeparation = 1024;

	g.ships[0] = addShip(g.cruiserData, Vec2i{0, 0}, randomFacing(), 0);
	sim::placeShipAtRandom(g.battle, g.ships[0], kMinSeparation);
	g.ships[1] = addShip(g.avengerData, Vec2i{0, 0}, randomFacing(), 1);
	sim::placeShipAtRandom(g.battle, g.ships[1], kMinSeparation);

	// The field goes in *after* the ships, not before.
	//
	// spawnPlanet rejects any position that overlaps something or sits in a
	// gravity well (misc.c:63-70), and it can only reject what already exists.
	// Placing it first meant it had nothing to avoid, so it could and did land
	// on a ship -- which, now that masks are the real silhouettes rather than
	// 12x12 blocks, means a ship starting *inside* a planet.
	const sim::CollisionMask *planetMask =
			g.world->maskFor(0) != nullptr ? g.world->maskFor(0) : &g.planetMask;
	const sim::CollisionMask *rockMask =
			g.rock->maskFor(0) != nullptr ? g.rock->maskFor(0) : &g.rockMask;

	// Spread over the arena, in display pixels. See kStarFieldWidth.
	for (Vec2i &s : g.stars)
		s = Vec2i{static_cast<std::int32_t>(
						  g.battle.rng().next() % kStarFieldWidth),
				static_cast<std::int32_t>(
						g.battle.rng().next() % kStarFieldHeight)};

	(void)sim::spawnPlanet(g.battle, planetMask);
	for (int i = 0; i < sim::kNumAsteroids; ++i)
		(void)sim::spawnAsteroid(g.battle, rockMask);
}

// The background, before anything in the arena.
//
// Stars do not zoom and they do not scale. They are meant to be at infinity,
// so putting them through camera.scale() was wrong twice over: it swelled
// them as the arena zoomed, and -- because the zoom drifts continuously as the
// ships close and separate -- it fed a moving divisor into every star's
// position, so the whole field shimmered under any pan or zoom. That was the
// jitter. The fix is not better rounding; it is that the zoom has no business
// in this calculation at all.
//
// What is left is a plain pan. Each plane is a screen-sized torus, and the
// camera's position enters it divided by 2^plane, so plane 0 slides a pixel
// for every display pixel the camera travels, plane 1 half that, plane 2 a
// quarter. Integer arithmetic throughout, one shift, nothing to round.
void
drawStars(Game &g)
{
	const Vec2i centre = g.camera.centre();

	const auto wrapTo = [](std::int32_t v, std::int32_t n) {
		v %= n;
		return v < 0 ? v + n : v;
	};

	std::size_t first = 0;
	for (int plane = 0; plane < kStarPlanes; ++plane)
	{
		const auto p = static_cast<std::size_t>(plane);
		const std::size_t count = static_cast<std::size_t>(kStarsPerPlane[p]);
		const Colour c = kStarColours[p];

		// World to display is a shift of two; the plane's own slowdown is
		// another `plane` on top of it.
		const std::int32_t ox = centre.x >> (sim::kOneShift + plane);
		const std::int32_t oy = centre.y >> (sim::kOneShift + plane);

		const game::SpriteSet *art = g.starArt;
		const std::size_t cel = kStarCels[p];
		const bool haveArt = art != nullptr && cel < art->frames.size()
				&& cel < art->masks.size();

		Extent2u size{1, 1};
		Vec2i hs{0, 0};
		if (haveArt)
		{
			size = art->masks[cel].size();
			hs = art->masks[cel].hotspot();
		}

		for (std::size_t i = first; i < first + count; ++i)
		{
			const std::int32_t sx = wrapTo(g.stars[i].x - ox, kStarFieldWidth);
			const std::int32_t sy = wrapTo(g.stars[i].y - oy, kStarFieldHeight);

			// Drawn up to four times so a star straddling the seam comes back
			// in on the other side instead of popping out of existence.
			for (int wx = 0; wx < 2; ++wx)
			{
				for (int wy = 0; wy < 2; ++wy)
				{
					const std::int32_t x = sx - wx * kStarFieldWidth - hs.x;
					const std::int32_t y = sy - wy * kStarFieldHeight - hs.y;
					if (x + static_cast<std::int32_t>(size.w) <= 0
							|| y + static_cast<std::int32_t>(size.h) <= 0
							|| x >= sim::kSpaceWidth || y >= sim::kSpaceHeight)
						continue;

					if (haveArt)
						g.window.draw(art->frames[cel], Vec2i{x, y}, size);
					else
						g.window.fillRect(
								Vec2i{x, y}, size, c.r, c.g, c.b);
				}
			}
		}

		first += count;
	}
}


void drawOverlay(Game &g);
void drawHud(Game &g);

void
draw(Game &g)
{
	g.window.clear(0x08, 0x08, 0x18);
	drawStars(g);

	// Every position goes through the camera, which goes through wrapDelta:
	// the arena is a torus eight screens across, so an element just over the
	// seam is a few pixels away, not eight screens away. Getting that wrong
	// makes things jump the width of the display as they cross.
	for (sim::EntityId id = g.battle.elements().front(); id.valid();
			id = g.battle.elements().next(id))
	{
		auto e = g.battle.get(id);
		if (e == nullptr)
			continue;

		// Exhaust: a single point stepping through the colour ramp as it
		// ages. lifeSpan counts down, so the ramp index counts up.
		if (e->kind == sim::ElementKind::IonTrail)
		{
			const std::size_t step = static_cast<std::size_t>(
					sim::kIonTrailLife - e->lifeSpan);
			if (step >= kIonRamp.size())
				continue;
			const Colour c = kIonRamp[step];
			const Vec2i at = g.camera.toScreen(e->current);
			g.window.fillRect(at, Extent2u{1, 1}, c.r, c.g, c.b);
			continue;
		}

		// A beam is a line between two points, not a sprite at one. Drawn
		// before the width/height work below, none of which applies.
		if (e->kind == sim::ElementKind::Laser)
		{
			g.window.drawLine(g.camera.toScreen(e->current),
					g.camera.toScreen(e->next), 0xFF, 0xFF, 0xFF);
			continue;
		}

		const Vec2i at = g.camera.toScreen(e->current);

		// The sprite shrinks with the zoom along with everything else -- one
		// zoom, one scale, no separate LOD path.
		const Extent2u mask =
				e->mask != nullptr ? e->mask->size() : Extent2u{8, 8};
		const std::int32_t w = std::max(
				1, g.camera.scale(sim::displayToWorld(
						   static_cast<std::int32_t>(mask.w))));
		const std::int32_t h = std::max(
				1, g.camera.scale(sim::displayToWorld(
						   static_cast<std::int32_t>(mask.h))));

		if (at.x + w < 0 || at.y + h < 0 || at.x - w > sim::kSpaceWidth
				|| at.y - h > sim::kSpaceHeight)
			continue;

		const Extent2u dest{static_cast<std::uint32_t>(w),
				static_cast<std::uint32_t>(h)};

		// A ship that has not arrived yet is not drawn -- only the shadows it
		// sheds are (tactrans.c:863). And a dead one is drawn as its own
		// explosion, growing over the frames it burns for.
		if (e->kind == sim::ElementKind::Ship)
		{
			if (any(e->flags & sim::ElementFlags::NonSolid)
					&& e->ship.crew > 0)
				continue;  // still warping in

			if (e->ship.crew == 0 && g.boom != nullptr && g.boom->valid())
			{
				const std::int32_t left = e->lifeSpan;
				const std::int32_t age = sim::kExplosionLife - left;
				const std::size_t frames = g.boom->frames.size();
				const std::size_t i = std::min(frames - 1,
						static_cast<std::size_t>(age) * frames
								/ static_cast<std::size_t>(sim::kExplosionLife));
				const std::int32_t size = std::max(1, w * 2);
				g.window.draw(g.boom->frames[i],
						Vec2i{at.x - size / 2, at.y - size / 2},
						Extent2u{static_cast<std::uint32_t>(size),
							static_cast<std::uint32_t>(size)});
				continue;
			}
		}

		if (const game::SpriteSet *set = spritesFor(g, *e); set != nullptr)
		{
			const std::size_t i = static_cast<std::size_t>(e->facing)
					% set->frames.size();

			// Draw from the cel's *hotspot*, not its centre.
			//
			// The hotspot is where the game considers the object to be, and
			// every cel has its own -- the Cruiser's sixteen facings are at
			// (7,19), (12,19), (16,15) and so on, because the ship is not
			// symmetric and each rendering sits differently in its box.
			// Centring instead makes the hull shift around as it turns, and
			// puts the sprite off the collision mask that is anchored to the
			// same hotspot.
			const Vec2i hs = set->masks[i].hotspot();
			const Extent2u src = set->masks[i].size();
			const std::int32_t ox = src.w != 0
					? static_cast<std::int32_t>(hs.x) * w
							/ static_cast<std::int32_t>(src.w)
					: w / 2;
			const std::int32_t oy = src.h != 0
					? static_cast<std::int32_t>(hs.y) * h
							/ static_cast<std::int32_t>(src.h)
					: h / 2;

			// A cloaking ship is a flat silhouette stepping through a fixed
			// colour ramp, not a faded sprite: white, cyan-white, dark cyan,
			// blue, dark blue, gone (ilwrath.c:250-285). Uncloaking runs the
			// same ramp backwards, and firing reverses it -- so an Avenger
			// that shoots while hidden lights itself up.
			// A warp-in shadow: the hull as a flat fill, stepping through the
			// exhaust ramp as it ages. Same STAMPFILL the cloak uses -- the C
			// uses that primitive for both, and for the same reason: what you
			// want is the ship's outline in one colour, not the ship.
			if (e->kind == sim::ElementKind::ShipShadow)
			{
				const std::size_t step = static_cast<std::size_t>(
						sim::kIonTrailLife - e->lifeSpan);
				if (step >= kIonRamp.size() || i >= set->silhouettes.size())
					continue;
				const Colour c = kIonRamp[step];
				g.window.drawTinted(set->silhouettes[i],
						Vec2i{at.x - ox, at.y - oy}, dest, c.r, c.g, c.b);
				continue;
			}

			const std::int32_t cloak = e->ship.cloakLevel;
			if (cloak > 0 && i < set->silhouettes.size())
			{
				// The last step is invisible, not black. The C fills with
				// BLACK_COLOR and that reads as gone against its own black
				// space -- but this renderer clears to a dark blue, so a black
				// fill leaves a ship-shaped hole, which is worse than no cloak
				// at all. Drawing nothing is what the C means.
				if (cloak >= sim::kCloakSteps - 1)
					continue;
				const Colour c = kCloakRamp[static_cast<std::size_t>(cloak)];
				g.window.drawTinted(set->silhouettes[i],
						Vec2i{at.x - ox, at.y - oy}, dest, c.r, c.g, c.b);
				continue;
			}

			g.window.draw(set->frames[i], Vec2i{at.x - ox, at.y - oy}, dest);
			continue;
		}

		const Colour c = colourFor(*e);
		g.window.fillRect(Vec2i{at.x - w / 2, at.y - h / 2}, dest, c.r, c.g,
				c.b);
	}

	drawHud(g);

	if (g.debugOverlay)
		drawOverlay(g);

	g.window.present();
}

// Crew and energy for both players, in the corners they own: player 0 top
// left, player 1 top right. Crew above energy, coloured to match the ship so
// there is nothing to read to know whose is whose.
//
// Drawn from the element, so a destroyed ship shows nothing rather than a
// stale number -- which is also how you tell "dead" from "at zero crew".
void
drawHud(Game &g)
{
	constexpr int kScale = 1;
	constexpr std::int32_t kMargin = 4;
	constexpr std::int32_t kLine = 7;

	for (std::size_t p = 0; p < g.ships.size(); ++p)
	{
		const auto e = g.battle.get(g.ships[p]);
		if (e == nullptr)
			continue;

		const Colour crewColour = colourFor(*e);
		constexpr Colour energyColour{0x60, 0xFF, 0xC0};

		// Player 0 hugs the left edge, player 1 the right. Both are drawn
		// right-aligned; only the anchor differs.
		const std::int32_t right = p == 0
				? kMargin + 3 * 4 * kScale
				: sim::kSpaceWidth - kMargin;

		drawNumber(g.window, e->ship.crew, Vec2i{right, kMargin}, kScale,
				crewColour);
		drawNumber(g.window, e->ship.energy, Vec2i{right, kMargin + kLine},
				kScale, energyColour);
	}
}

// The collision overlay: what touched, where, and what the response did.
//
// Reads Battle::collisions() rather than anything of its own, so what is drawn
// is exactly what the simulation resolved -- an overlay that recomputed the
// contact point could agree with itself while disagreeing with the physics,
// which would be worse than no overlay.
void
drawOverlay(Game &g)
{
	// Mask bounds, so it is visible when a silhouette is not what you expect.
	for (sim::EntityId id = g.battle.elements().front(); id.valid();
			id = g.battle.elements().next(id))
	{
		const auto e = g.battle.get(id);
		if (e == nullptr || e->mask == nullptr)
			continue;

		const Vec2i at = g.camera.toScreen(e->current);
		const Extent2u m = e->mask->size();
		const std::int32_t w = g.camera.scale(
				sim::displayToWorld(static_cast<std::int32_t>(m.w)));
		const std::int32_t h = g.camera.scale(
				sim::displayToWorld(static_cast<std::int32_t>(m.h)));
		const Vec2i hs = e->mask->hotspot();
		const std::int32_t ox = m.w != 0
				? static_cast<std::int32_t>(hs.x) * w
						/ static_cast<std::int32_t>(m.w)
				: w / 2;
		const std::int32_t oy = m.h != 0
				? static_cast<std::int32_t>(hs.y) * h
						/ static_cast<std::int32_t>(m.h)
				: h / 2;

		const Vec2i tl{at.x - ox, at.y - oy};
		const Vec2i br{tl.x + w, tl.y + h};
		g.window.drawLine(tl, Vec2i{br.x, tl.y}, 0x30, 0x60, 0x30);
		g.window.drawLine(Vec2i{br.x, tl.y}, br, 0x30, 0x60, 0x30);
		g.window.drawLine(br, Vec2i{tl.x, br.y}, 0x30, 0x60, 0x30);
		g.window.drawLine(Vec2i{tl.x, br.y}, tl, 0x30, 0x60, 0x30);

		// The hotspot itself, which is where the game thinks the thing *is*.
		g.window.drawLine(Vec2i{at.x - 2, at.y}, Vec2i{at.x + 2, at.y}, 0x40,
				0xFF, 0x40);
		g.window.drawLine(Vec2i{at.x, at.y - 2}, Vec2i{at.x, at.y + 2}, 0x40,
				0xFF, 0x40);
	}

	// Contact points and response vectors. Velocities are scaled up because a
	// frame of travel is a handful of world units and an unscaled arrow would
	// be a dot.
	constexpr std::int32_t kVectorGain = 8;
	for (const Game::Mark &mark : g.marks)
	{
		const std::int64_t age =
				static_cast<std::int64_t>(g.battle.frame()) - mark.frame;
		if (age > kMarkLife)
			continue;

		const Vec2i at = g.camera.toScreen(mark.event.at);
		g.window.drawLine(Vec2i{at.x - 4, at.y - 4}, Vec2i{at.x + 4, at.y + 4},
				0xFF, 0x30, 0x30);
		g.window.drawLine(Vec2i{at.x - 4, at.y + 4}, Vec2i{at.x + 4, at.y - 4},
				0xFF, 0x30, 0x30);

		// Before in dim, after in bright: the difference *is* the response.
		const auto arrow = [&](Vec2i v, std::uint8_t r, std::uint8_t gg,
								   std::uint8_t b) {
			g.window.drawLine(at,
					Vec2i{at.x + g.camera.scale(v.x * kVectorGain),
						at.y + g.camera.scale(v.y * kVectorGain)},
					r, gg, b);
		};
		arrow(mark.event.beforeA, 0x60, 0x60, 0x90);
		arrow(mark.event.beforeB, 0x60, 0x60, 0x90);
		arrow(mark.event.afterA, 0x60, 0xC0, 0xFF);
		arrow(mark.event.afterB, 0xFF, 0xC0, 0x60);
	}
}

// One pass of the outer loop: drain input, run whatever simulation time is
// owed, draw once. Never more than one draw per call, and never a simulation
// step that is not due -- the two rates are independent and only this function
// knows both.
void
iterate(Game &g)
{
	if (!g.window.pump(g.players, platform::defaultBindings()))
	{
		g.running = false;
		return;
	}

	const int steps = g.pacer.stepsDue(g.window.now());
	for (int i = 0; i < steps; ++i)
	{
		// Input is consumed once per simulation step, not once per display
		// frame. That is what makes a tap land on exactly one step.
		for (std::size_t p = 0; p < g.players.size(); ++p)
		{
			const input::Buttons b = g.players[p].consume();
			if (b.test(Button::Escape))
				g.running = false;
			if (b.test(Button::Debug))
				g.debugOverlay = !g.debugOverlay;
			if (auto ship = g.battle.get(g.ships[p]); ship != nullptr)
				ship->ship.input = toShipInput(b);
		}
		g.battle.step();

		// Collected every frame regardless of the overlay, because the events
		// are the simulation's and only the drawing is optional.
		for (const sim::CollisionEvent &c : g.battle.collisions())
		{
			g.marks.push_back(
					Game::Mark{c, static_cast<std::int64_t>(g.battle.frame())});

			// And heard. This is the second consumer the events were recorded
			// for rather than merely acted on -- the overlay was the first.
			//
			// Which boom depends on how hard the hit was: the C picks
			// TARGET_DAMAGED_FOR_1_PT + (damage >> 1), capped at the 6-plus
			// slot (weapon.c:168-172, ship.c:369-371). battle.snd lists them
			// in that order after getcrew and shipdies, so slot 2 is a scratch
			// and slot 5 is a nuke going off. Playing slot 2 for everything,
			// which is what this did first, makes every impact sound trivial.
			std::int32_t damage = 0;
			for (const sim::EntityId side : {c.a, c.b})
				if (const auto el = g.battle.get(side); el != nullptr)
					damage = std::max(damage, el->damage);

			const std::size_t slot = std::min<std::size_t>(
					kBoomFirstSlot + static_cast<std::size_t>(damage >> 1),
					kBoomFirstSlot + 3);
			if (g.battleSounds.size() > slot)
				g.audio.play(g.battleSounds[slot], kEffectGain);
		}

		// Weapons fired this frame, and beams. Counted by watching the list
		// rather than by a callback from the simulation, because a sound is
		// presentation and the simulation should not know it exists.
		std::size_t shots = 0;
		std::size_t beams = 0;
		for (sim::EntityId e = g.battle.elements().front(); e.valid();
				e = g.battle.elements().next(e))
		{
			const auto el = g.battle.get(e);
			if (el == nullptr || !any(el->flags & sim::ElementFlags::Appearing))
				continue;
			if (el->kind == sim::ElementKind::Weapon)
			{
				++shots;
				// Whose weapon: the Cruiser's nuke and the Avenger's flame are
				// different sounds, and both are slot 0 of their own ship's
				// .snd. Playing the Cruiser's for everything made the flame
				// sound like a missile launch every frame.
				const auto &set = el->playerNr == 0 ? g.cruiserSounds
													: g.avengerSounds;
				if (!set.empty())
					g.audio.play(set[0], kEffectGain);
			}
			else if (el->kind == sim::ElementKind::Laser)
			{
				++beams;
				// cruiser.snd slot 1: secondary.wav, the point-defence laser
				// (human.c:232-234).
				if (g.cruiserSounds.size() > 1)
					g.audio.play(g.cruiserSounds[1], kEffectGain);
			}
		}
		g.lastShots = shots;
		(void)beams;
	}

	// Drop what has aged out. Done here rather than while drawing so the list
	// does not grow without bound when the overlay is off.
	const std::int64_t now = static_cast<std::int64_t>(g.battle.frame());
	std::erase_if(g.marks, [now](const Game::Mark &m) {
		return now - m.frame > kMarkLife;
	});

	// A ship is gone when its element is: doDamage sets life_span to 0 and the
	// step loop reaps it. Deciding this from the element rather than from a
	// crew count means a ship destroyed by any means counts, not just one shot
	// to death.
	if (g.winner < 0)
	{
		const bool alive0 = g.battle.get(g.ships[0]) != nullptr;
		const bool alive1 = g.battle.get(g.ships[1]) != nullptr;
		if (!alive0 || !alive1)
		{
			g.winner = alive0 ? 0 : (alive1 ? 1 : 2);
			// battle.snd slot 1: shipdies.wav (tactrans.c:723-726).
			if (g.battleSounds.size() > 1)
				g.audio.play(g.battleSounds[1], kEffectGain);
			g.endedAtFrame = static_cast<std::int64_t>(g.battle.frame());
			if (g.winner == 2)
				std::printf("mutual destruction\n");
			else
				std::printf("player %d wins\n", g.winner);
			std::fflush(stdout);
		}
	}
	else if (static_cast<std::int64_t>(g.battle.frame()) - g.endedAtFrame
			> kBattleHz * 2)
	{
		// Two seconds to watch the wreck, then out. A menu goes here in M2.
		g.running = false;
	}

	// The camera is presentation, so it follows the simulation rather than
	// being part of it -- recomputed once per displayed frame from whatever
	// the last step left behind.
	std::array<Vec2i, 2> eyes{};
	std::size_t living = 0;
	for (const sim::EntityId id : g.ships)
		if (auto e = g.battle.get(id); e != nullptr)
			eyes[living++] = e->current;
	if (living > 0)
		g.camera.follow(std::span<const Vec2i>{eyes.data(), living});

	draw(g);
}

Game *g_game = nullptr;

void
iterateOnce()
{
	iterate(*g_game);
}

}  // namespace

int
main(int argc, char **argv)
{
	Game game;
	setUp(game, findContent(argc > 1 ? std::filesystem::path(argv[1])
									 : std::filesystem::path{}));
	g_game = &game;

#ifdef __EMSCRIPTEN__
	// Let the browser drive. 0 means "use requestAnimationFrame", which is the
	// display rate and is exactly why the Pacer exists: we do not get to ask
	// for 24 Hz here.
	emscripten_set_main_loop(iterateOnce, 0, 1);
#else
	while (game.running)
		iterateOnce();
#endif
	return 0;
}
