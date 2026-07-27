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

// Everything the frame needs. A struct rather than globals so the Emscripten
// driver has something to hand back to iterate().
struct Game
{
	platform::Platform window{"The Ur-Quan Masters -- melee",
			Extent2u{static_cast<std::uint32_t>(sim::kSpaceWidth),
				static_cast<std::uint32_t>(sim::kSpaceHeight)},
			3};

	sim::Battle battle{0x2A5B};
	game::Camera camera;
	Pacer pacer;
	std::array<InputAccumulator, 2> players;

	// Everything content-addressed. Resources owns the sets and the masks
	// inside them, so both outlive the battle -- which is what Element::mask
	// assumes.
	game::Resources content;

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
};

// How long a contact point stays on screen, in simulation frames.
constexpr std::int64_t kMarkLife = 24;

// Which sprite set an element draws from. Ownership decides the weapon art,
// because a missile belongs to whoever fired it.
const game::SpriteSet *
spritesFor(const Game &g, const sim::Element &e) noexcept
{
	const game::SpriteSet *set = nullptr;
	switch (e.kind)
	{
		case sim::ElementKind::Ship:
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
		e.preProcess = sim::shipPreProcess;
		e.postProcess = sim::shipPostProcess;
		e.onCollision = sim::solidCollision;
		return g.battle.spawnBack(std::move(e));
	};

	// 1024 world units apart, which the continuous camera renders at 2:1 --
	// far enough to fly at each other, close enough to see both. A quarter of
	// the arena apart is the 4:1 clamp exactly, which pins them to the screen
	// edges and looks like a bug even though it is the camera working.
	constexpr std::int32_t kStartGap = 512;
	g.ships[0] = addShip(g.cruiserData,
			Vec2i{sim::kLogSpaceWidth / 2 - kStartGap, sim::kLogSpaceHeight / 2},
			4, 0);
	g.ships[1] = addShip(g.avengerData,
			Vec2i{sim::kLogSpaceWidth / 2 + kStartGap, sim::kLogSpaceHeight / 2},
			12, 1);

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

	(void)sim::spawnPlanet(g.battle, planetMask);
	for (int i = 0; i < sim::kNumAsteroids; ++i)
		(void)sim::spawnAsteroid(g.battle, rockMask);
}

void drawOverlay(Game &g);

void
draw(Game &g)
{
	g.window.clear(0x08, 0x08, 0x18);

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

			g.window.draw(set->frames[i], Vec2i{at.x - ox, at.y - oy}, dest);
			continue;
		}

		const Colour c = colourFor(*e);
		g.window.fillRect(Vec2i{at.x - w / 2, at.y - h / 2}, dest, c.r, c.g,
				c.b);
	}

	if (g.debugOverlay)
		drawOverlay(g);

	g.window.present();
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
			g.marks.push_back(
					Game::Mark{c, static_cast<std::int64_t>(g.battle.frame())});
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
