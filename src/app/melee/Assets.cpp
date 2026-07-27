// Copyright the Ur-Quan Masters contributors. GPL-2.0-or-later.

#include "app/melee/Assets.hpp"
#include "app/melee/Game.hpp"

#include "game/Melee.hpp"
#include "game/Ships.hpp"
#include "platform/Platform.hpp"

#include <cassert>
#include <cstdio>
#include <filesystem>
#include <string_view>

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

	// Today's fixed roster, by catalog key. What ships exist and what they
	// load is game/Ships.cpp's business; only the match-up is decided here.
	const game::ShipDef *cru = game::findShip("earthling.cruiser");
	const game::ShipDef *ave = game::findShip("ilwrath.avenger");
	assert(cru != nullptr && ave != nullptr);

	// Addressed by resource id, not by path. uqm.rmp is the only link between
	// a name and a file, and it is what an addon overrides -- see
	// game/Resources.hpp.
	const auto load = [&](std::string_view id) -> const game::SpriteSet * {
		const game::SpriteSet &set = g.content.sprites(g.window, id);
		if (!set.valid() && g.content.valid())
			std::fprintf(stderr, "content: could not load %.*s\n",
					static_cast<int>(id.size()), id.data());
		return &set;
	};

	g.cruiser = load(cru->art.ship);
	g.avenger = load(ave->art.ship);
	g.nuke = load(cru->art.weapon);
	g.flame = load(ave->art.weapon);
	g.rock = load(game::kMeleeArt.asteroid);
	g.blast = load(game::kMeleeArt.blast);
	g.boom = load(game::kMeleeArt.boom);
	g.world = load(game::kMeleeArt.planet);
	g.starArt = load(game::kMeleeArt.stars);

	const auto loadSounds = [&](std::string_view id) {
		const std::span<const platform::Sound> set =
				g.content.sounds(g.audio, id);
		std::size_t ok = 0;
		for (const platform::Sound &snd : set)
			ok += snd.valid() ? 1 : 0;
		std::fprintf(stderr, "audio: %.*s -> %zu/%zu loaded\n",
				static_cast<int>(id.size()), id.data(), ok, set.size());
		return set;
	};
	g.cruiserSounds = loadSounds(cru->art.sounds);
	g.avengerSounds = loadSounds(ave->art.sounds);
	g.battleSounds = loadSounds(game::kMeleeArt.battleSounds);
	if (!g.audio.valid())
		std::fprintf(stderr, "audio: no device; the game runs silent\n");

	// Descriptors first, then the content-derived masks on top: skipping
	// these leaves a default ShipSpec with thrust.max = 0 and turnWait = 0,
	// a ship that cannot accelerate and spins every frame.
	g.cruiserData = *cru->spec;
	g.avengerData = *ave->spec;

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
