// Copyright the Ur-Quan Masters contributors. GPL-2.0-or-later.

#include "app/melee/Game.hpp"
#include "app/melee/Assets.hpp"
#include "app/melee/Draw.hpp"
#include "app/melee/Sound.hpp"

#include "engine/core/Types.hpp"
#include "game/Melee.hpp"
#include "sim/Damage.hpp"
#include "sim/Field.hpp"
#include "sim/ShipSystems.hpp"

#include <array>
#include <chrono>
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

u32
battleSeed()
{
	const auto t = static_cast<u64>(
			std::chrono::steady_clock::now().time_since_epoch().count());
	const auto seed = static_cast<u32>((t ^ (t >> 32)) & 0x7FFFFFFFu) | 1u;
	std::fprintf(stderr, "battle seed: %lu\n", static_cast<unsigned long>(seed));
	return seed;
}

namespace {

void
setUpBattle(Game &g)
{
	auto &cfg = g.battle.context<BattleConfig>();
	auto &match = g.battle.context<MatchState>();

	// Random facings and random positions, as the C does (ship.c:456, 473).
	// The facing has to be chosen before the ship is spawned, because the
	// collision mask is per-facing and placement tests that mask.
	const auto randomFacing = [&g] {
		return sim::Facing(static_cast<int>(g.battle.rng().next() & 0xFF));
	};

	// Minimum separation so a melee doesn't open with the ships touching
	// (design-notes.md V7): 1024 world units, far enough to close on each
	// other, close enough for the camera to hold both.
	constexpr i32 kMinSeparation = 1024;

	// The real silhouette when the art loaded, Resources' placeholder block
	// when it did not -- either way maskFor cannot miss. Per-pixel collision
	// against a square is not per-pixel collision, so a missing mask changes
	// how the ships actually touch.
	const sim::Facing facing0 = randomFacing();
	match.shipIds[0] = sim::spawnPlayerShip(g.battle, cfg.shipData[0],
			g.content.sprites(g.window, cfg.roster[0]->art.ship)
					.maskFor(facing0.raw()),
			Vec2i{0, 0}, facing0, 0, /*warpIn=*/true);
	sim::placeShipAtRandom(g.battle, match.shipIds[0], kMinSeparation);

	const sim::Facing facing1 = randomFacing();
	match.shipIds[1] = sim::spawnPlayerShip(g.battle, cfg.shipData[1],
			g.content.sprites(g.window, cfg.roster[1]->art.ship)
					.maskFor(facing1.raw()),
			Vec2i{0, 0}, facing1, 1, /*warpIn=*/true);
	sim::placeShipAtRandom(g.battle, match.shipIds[1], kMinSeparation);

	// The field spawns after the ships: spawnPlanet rejects any position
	// overlapping something or in a gravity well (misc.c:63-70), and can
	// only reject what already exists to avoid.
	const sim::CollisionMask *planetMask =
			g.content.sprites(g.window, game::kMeleeArt.planet).maskFor(0);
	const sim::CollisionMask *rockMask =
			g.content.sprites(g.window, game::kMeleeArt.asteroid).maskFor(0);

	// The starfield: one entity (review-007 §3), positions spread over the
	// arena in display pixels (see kStarFieldWidth), populated once here.
	Starfield sf;
	for (Vec2i &s : sf.stars)
		s = Vec2i{static_cast<i32>(g.battle.rng().next() % kStarFieldWidth),
				static_cast<i32>(g.battle.rng().next() % kStarFieldHeight)};
	g.battle.attach<Starfield>(g.battle.create(), std::move(sf));

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
	g.battle.setContext<MatchState>(MatchState{});
	g.battle.setContext<DebugToggles>(DebugToggles{});
	g.battle.setContext<game::Camera>(game::Camera{});

	loadAssets(g, content);
	setUpBattle(g);

	// The initial furniture -- both ships, the asteroids, the planet -- gets
	// its Visual now; everything spawned later is caught in iterate(). A
	// pure join now (review-007 W4b): Allegiance is enough to find every
	// spawned element (attached uniformly, same call as Order), so the
	// join replaces the get-then-null-check without needing Element::kind
	// (review-007 W7 -- visualFor selects art from composition instead).
	g.battle.eachOrdered<sim::Allegiance>(
			[&g](sim::EntityId id, sim::Allegiance &a) {
				g.battle.attach<Visual>(id, visualFor(g, id, a.playerNr));
			});
}

void
iterate(Game &g)
{
	if (!g.window.pump(g.players, platform::defaultBindings()))
	{
		g.running = false;
		return;
	}

	auto &debug = g.battle.context<DebugToggles>();
	auto &match = g.battle.context<MatchState>();

	const int steps = g.pacer.stepsDue(g.window.now());
	for (int i = 0; i < steps; ++i)
	{
		// Input is consumed once per step, not once per frame, so a tap
		// lands exactly once (design-notes.md D7, V6).
		for (usize p = 0; p < g.players.size(); ++p)
		{
			const input::Buttons b = g.players[p].consume();
			if (b.test(Button::Escape))
				g.running = false;
			if (p == 0)
			{
				const bool debugDown = b.test(Button::Debug);
				if (debugDown && !debug.wasDown)
					debug.overlay = !debug.overlay;
				debug.wasDown = debugDown;
			}
			if (sim::Input *in = g.battle.find<sim::Input>(match.shipIds[p]))
				in->buttons = toShipInput(b);
		}
		g.battle.step();

		// Collision events are step()'s output regardless of the overlay
		// (design-notes.md D5); only drawing them is optional. Each becomes
		// a bare Mark entity (review-007 §3): outside the sim's element
		// count, reaped by age below rather than through the Doomed/reap
		// protocol a sim element would use.
		for (const sim::CollisionEvent &c : g.battle.collisions())
			g.battle.attach<Mark>(g.battle.create(), Mark{c, g.battle.frame()});

		playStepSounds(g);

		// Everything the sim spawned this step gets a Visual before it is
		// ever drawn. Guarded: an element executed for spawning inside
		// something (killOverlapSpawn) is already destroyed by the time its
		// SpawnEvent is read, and a dead entity cannot carry a component.
		for (const sim::SpawnEvent &sp : g.battle.spawns())
		{
			if (g.battle.alive(sp.id))
				g.battle.attach<Visual>(
						sp.id, visualFor(g, sp.id, sp.playerNr));
		}
	}

	// Drop what has aged out. Done here rather than while drawing so the
	// registry does not grow without bound when the overlay is off. Marks
	// are app-owned bare entities (Battle::create()), so ageing them out is
	// this loop's job, not the sim's Doomed/reap protocol; collected first
	// since destroying mid-view is not safe.
	std::vector<sim::EntityId> agedOut;
	g.battle.view<Mark>().each([&](sim::EntityId id, Mark &m) {
		if (g.battle.frame() - m.bornFrame > kMarkLife)
			agedOut.push_back(id);
	});
	for (sim::EntityId id : agedOut)
		g.battle.destroy(id);

	// A ship is gone when its element is: doDamage zeroes life_span and the
	// step loop reaps it, so any means of destruction counts, not just one
	// shot to death.
	if (match.winner < 0)
	{
		const bool alive0 = g.battle.alive(match.shipIds[0]);
		const bool alive1 = g.battle.alive(match.shipIds[1]);
		if (!alive0 || !alive1)
		{
			match.winner = alive0 ? 0 : (alive1 ? 1 : 2);
			match.endedAtFrame = static_cast<i64>(g.battle.frame());
			if (match.winner == 2)
				std::printf("mutual destruction\n");
			else
				std::printf("player %d wins\n", match.winner);
			std::fflush(stdout);
		}
	}
	else if (static_cast<i64>(g.battle.frame()) - match.endedAtFrame
			> kBattleHz * 2)
	{
		// Two seconds to watch the wreck, then out. A menu goes here in M2.
		g.running = false;
	}

	// The camera is presentation, so it follows the simulation rather than
	// being part of it -- recomputed once per displayed frame from whatever
	// the last step left behind.
	std::array<Vec2i, 2> eyes{};
	usize living = 0;
	for (const sim::EntityId id : match.shipIds)
		if (g.battle.alive(id))
			eyes[living++] = g.battle.find<sim::Position>(id)->current;
	if (living > 0)
		g.battle.context<game::Camera>().follow(
				std::span<const Vec2i>{eyes.data(), living});

	draw(g);
}

}  // namespace uqm::melee
