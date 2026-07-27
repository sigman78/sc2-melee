// Copyright the Ur-Quan Masters contributors. GPL-2.0-or-later.
//
// The M1 vertical slice: Human Cruiser against Ilwrath Avenger, two players at
// one keyboard, on a real window.
//
// Nothing is drawn from content yet -- ships and shots are coloured rectangles
// sized from their collision masks. That is deliberate for this step. The
// point of the slice is to prove the *shape* of the loop, and a rectangle in
// the right place at the right time proves it as well as a sprite does while
// failing much more loudly when it is wrong.
//
// The loop is the plan's `main` + `iterate()`: one function that advances
// everything by whatever time has passed, called from a driver that differs
// per platform. Emscripten cannot own the outer while-loop -- it has to return
// to the browser between frames -- and writing that shape from the start is
// what stops the desktop build growing a structure the web build cannot use.

#include "engine/core/Pacing.hpp"
#include "engine/input/Input.hpp"
#include "game/Camera.hpp"
#include "game/ShipSprites.hpp"
#include "platform/Platform.hpp"
#include "sim/Battle.hpp"
#include "sim/Damage.hpp"
#include "sim/Field.hpp"
#include "sim/Ship.hpp"
#include "sim/World.hpp"

#include <algorithm>
#include <array>
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

	// Sprite sets, and the masks that come with them. Both outlive the
	// battle, which is what Element::mask assumes.
	game::ShipSprites cruiser;
	game::ShipSprites avenger;

	// Fallbacks for anything without art yet -- shots, rocks, the planet.
	sim::CollisionMask shipMask = block(12, 12);
	sim::CollisionMask shotMask = block(3, 3);
	sim::CollisionMask rockMask = block(8, 8);
	sim::CollisionMask planetMask = block(28, 28);

	std::array<sim::EntityId, 2> ships{};
	bool running = true;
};

// Which sprite set an element draws from. Only ships have art so far.
const game::ShipSprites *
spritesFor(const Game &g, const sim::Element &e) noexcept
{
	if (e.kind != sim::ElementKind::Ship)
		return nullptr;
	const game::ShipSprites *set = e.playerNr == 0 ? &g.cruiser : &g.avenger;
	return set->valid() ? set : nullptr;
}

void
setUp(Game &g, const std::filesystem::path &content)
{
	const std::filesystem::path ct = content / "base/uqm.ct";
	g.cruiser = game::loadShipSprites(
			g.window, content / "base/ships/human/cruiser-big.ani", ct);
	g.avenger = game::loadShipSprites(
			g.window, content / "base/ships/ilwrath/avenger-big.ani", ct);

	// The planet first, then the asteroids -- init.c's order, and the RNG
	// stream depends on it.
	(void)sim::spawnPlanet(g.battle, &g.planetMask);
	for (int i = 0; i < sim::kNumAsteroids; ++i)
		(void)sim::spawnAsteroid(g.battle, &g.rockMask);

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
		const game::ShipSprites *set = player == 0 ? &g.cruiser : &g.avenger;
		const sim::CollisionMask *m = set->maskFor(facing);
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
	g.ships[0] = addShip(sim::earthlingCruiser(),
			Vec2i{sim::kLogSpaceWidth / 2 - kStartGap, sim::kLogSpaceHeight / 2},
			4, 0);
	g.ships[1] = addShip(sim::ilwrathAvenger(),
			Vec2i{sim::kLogSpaceWidth / 2 + kStartGap, sim::kLogSpaceHeight / 2},
			12, 1);
}

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
		const sim::Element *e = g.battle.get(id);
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

		if (const game::ShipSprites *set = spritesFor(g, *e); set != nullptr)
		{
			const std::size_t i = static_cast<std::size_t>(e->facing)
					% set->frames.size();
			g.window.draw(set->frames[i], Vec2i{at.x - w / 2, at.y - h / 2},
					dest);
			continue;
		}

		const Colour c = colourFor(*e);
		g.window.fillRect(Vec2i{at.x - w / 2, at.y - h / 2}, dest, c.r, c.g,
				c.b);
	}

	g.window.present();
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
			if (sim::Element *ship = g.battle.get(g.ships[p]); ship != nullptr)
				ship->ship.input = toShipInput(b);
		}
		g.battle.step();
	}

	// The camera is presentation, so it follows the simulation rather than
	// being part of it -- recomputed once per displayed frame from whatever
	// the last step left behind.
	std::array<Vec2i, 2> eyes{};
	std::size_t living = 0;
	for (const sim::EntityId id : g.ships)
		if (const sim::Element *e = g.battle.get(id); e != nullptr)
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
	// Where the content tree is. Defaults to the one in this repository, so
	// running it from the build directory just works during development.
	const std::filesystem::path content =
			argc > 1 ? std::filesystem::path(argv[1])
					 : std::filesystem::path("sc2/content");

	Game game;
	setUp(game, content);
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
