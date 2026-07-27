// Copyright the Ur-Quan Masters contributors. GPL-2.0-or-later.

#include "app/melee/Game.hpp"
#include "app/melee/Assets.hpp"
#include "app/melee/Draw.hpp"
#include "app/melee/Sound.hpp"

#include "game/Melee.hpp"
#include "sim/Damage.hpp"
#include "sim/Field.hpp"

#include <array>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <span>
#include <utility>
#include <vector>

namespace uqm::melee {

namespace {

using uqm::input::Button;
using uqm::input::InputAccumulator;

// What the accumulator reports, in the simulation's vocabulary. Lives here
// because only this layer knows both. Left beats right (battle.c:201-204's
// else-if): pressing both doesn't turn twice as fast, or not at all.
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

}  // namespace

std::uint32_t
battleSeed()
{
	const auto t = static_cast<std::uint64_t>(
			std::chrono::steady_clock::now().time_since_epoch().count());
	const auto seed =
			static_cast<std::uint32_t>((t ^ (t >> 32)) & 0x7FFFFFFFu) | 1u;
	std::fprintf(stderr, "battle seed: %lu\n", static_cast<unsigned long>(seed));
	return seed;
}

namespace {

void
setUpBattle(Game &g)
{
	const auto addShip = [&g](const sim::ShipSpec &data, Vec2i at,
							  sim::Facing facing, int player) {
		sim::Element e;
		e.kind = sim::ElementKind::Ship;
		e.flags = sim::ElementFlags::PlayerShip | sim::ElementFlags::IgnoreSimilar;
		e.current = at;
		e.next = at;
		e.facing = facing;
		e.playerNr = player;
		e.mass = data.mass;

		// The real silhouette when the art loaded, Resources' placeholder
		// block when it did not -- either way maskFor cannot miss. Per-pixel
		// collision against a square is not per-pixel collision, so a
		// missing mask changes how the ships actually touch.
		const game::SpriteSet &set = g.content.sprites(g.window,
				g.roster[static_cast<std::size_t>(player)]->art.ship);
		e.mask = set.maskFor(facing.raw());
		// Warping in, not simply present. shipTransition hands over to
		// shipPreProcess once the ship has arrived.
		e.preProcess = sim::shipTransition;
		e.postProcess = nullptr;
		e.onCollision = sim::solidCollision;
		const sim::EntityId id = g.battle.spawnBack(std::move(e));
		g.battle.attachShip(id, &data);
		return id;
	};

	// Random facings and random positions, as the C does (ship.c:456, 473).
	// The facing has to be chosen before the ship is spawned, because the
	// collision mask is per-facing and placement tests that mask.
	const auto randomFacing = [&g] {
		return sim::Facing(static_cast<int>(g.battle.rng().next() & 0xFF));
	};

	// Minimum separation so a melee doesn't open with the ships touching
	// (design-notes.md V7): 1024 world units, far enough to close on each
	// other, close enough for the camera to hold both.
	constexpr std::int32_t kMinSeparation = 1024;

	g.ships[0] = addShip(g.shipData[0], Vec2i{0, 0}, randomFacing(), 0);
	sim::placeShipAtRandom(g.battle, g.ships[0], kMinSeparation);
	g.ships[1] = addShip(g.shipData[1], Vec2i{0, 0}, randomFacing(), 1);
	sim::placeShipAtRandom(g.battle, g.ships[1], kMinSeparation);

	// The field spawns after the ships: spawnPlanet rejects any position
	// overlapping something or in a gravity well (misc.c:63-70), and can
	// only reject what already exists to avoid.
	const sim::CollisionMask *planetMask =
			g.content.sprites(g.window, game::kMeleeArt.planet).maskFor(0);
	const sim::CollisionMask *rockMask =
			g.content.sprites(g.window, game::kMeleeArt.asteroid).maskFor(0);

	// Spread over the arena, in display pixels. See kStarFieldWidth.
	for (Vec2i &s : g.stars)
		s = Vec2i{static_cast<std::int32_t>(
						  g.battle.rng().next() % kStarFieldWidth),
				static_cast<std::int32_t>(
						g.battle.rng().next() % kStarFieldHeight)};

	// Asteroids first, then the planet -- init.c:228-233's order. The planet's
	// placement loop rejects anything it would overlap, so it has to be able
	// to see the asteroids (and the ships, above) when it rolls.
	for (int i = 0; i < sim::kNumAsteroids; ++i)
		(void)sim::spawnAsteroid(g.battle, rockMask);
	(void)sim::spawnPlanet(g.battle, planetMask);
}

}  // namespace

void
setUp(Game &g, const std::filesystem::path &content)
{
	loadAssets(g, content);
	setUpBattle(g);

	// The initial furniture -- both ships, the asteroids, the planet -- gets
	// its Visual now; everything spawned later is caught in iterate().
	for (sim::EntityId id = g.battle.front(); id != sim::kNoEntity;
			id = g.battle.next(id))
	{
		auto e = g.battle.get(id);
		if (e == nullptr)
			continue;
		g.battle.registry().emplace<Visual>(
				id, visualFor(g, e->kind, e->playerNr));
	}
}

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
		// Input is consumed once per step, not once per frame, so a tap
		// lands exactly once (design-notes.md D7, V6).
		for (std::size_t p = 0; p < g.players.size(); ++p)
		{
			const input::Buttons b = g.players[p].consume();
			if (b.test(Button::Escape))
				g.running = false;
			if (p == 0)
			{
				const bool debugDown = b.test(Button::Debug);
				if (debugDown && !g.debugWasDown)
					g.debugOverlay = !g.debugOverlay;
				g.debugWasDown = debugDown;
			}
			if (sim::ShipState *s = g.battle.ship(g.ships[p]))
				s->input = toShipInput(b);
		}
		g.battle.step();

		// Collision events are step()'s output regardless of the overlay
		// (design-notes.md D5); only drawing them is optional.
		for (const sim::CollisionEvent &c : g.battle.collisions())
		{
			g.marks.push_back(
					Game::Mark{c, static_cast<std::int64_t>(g.battle.frame())});
		}

		playStepSounds(g);

		// Everything the sim spawned this step gets a Visual before it is
		// ever drawn. Guarded: an element executed for spawning inside
		// something (killOverlapSpawn) is already destroyed by the time its
		// SpawnEvent is read, and a dead entity cannot carry a component.
		for (const sim::SpawnEvent &sp : g.battle.spawns())
		{
			if (g.battle.alive(sp.id))
				g.battle.registry().emplace<Visual>(
						sp.id, visualFor(g, sp.kind, sp.playerNr));
		}
	}

	// Drop what has aged out. Done here rather than while drawing so the list
	// does not grow without bound when the overlay is off.
	const std::int64_t now = static_cast<std::int64_t>(g.battle.frame());
	std::erase_if(g.marks, [now](const Game::Mark &m) {
		return now - m.frame > kMarkLife;
	});

	// A ship is gone when its element is: doDamage zeroes life_span and the
	// step loop reaps it, so any means of destruction counts, not just one
	// shot to death.
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

}  // namespace uqm::melee
