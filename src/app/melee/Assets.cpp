// Copyright the Ur-Quan Masters contributors. GPL-2.0-or-later.

#include "app/melee/Assets.hpp"
#include "app/melee/Game.hpp"

#include "platform/Platform.hpp"
#include "sim/Ship.hpp"

#include <cstdio>
#include <filesystem>

namespace uqm::melee {

std::filesystem::path
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
loadAssets(Game &g, const std::filesystem::path &content)
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

	// Descriptors first, then the content-derived masks on top: skipping
	// these leaves a default ShipSpec with thrust.max = 0 and turnWait = 0,
	// a ship that cannot accelerate and spins every frame.
	g.cruiserData = sim::earthlingCruiser();
	g.avengerData = sim::ilwrathAvenger();

	g.cruiserData.facingMasks = g.cruiser->masks;
	g.avengerData.facingMasks = g.avenger->masks;
	g.cruiserData.weapon.masks = g.nuke->masks;
	g.avengerData.weapon.masks = g.flame->masks;

	if (!g.cruiserData.valid() || !g.avengerData.valid())
	{
		std::fprintf(stderr,
				"ship: a descriptor was never filled in -- the ships will not "
				"fly. This is a setup bug, not a control one.\n");
	}
}

}  // namespace uqm::melee
