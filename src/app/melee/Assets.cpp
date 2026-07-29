// Copyright the Ur-Quan Masters contributors. GPL-2.0-or-later.

#include "app/melee/Assets.hpp"
#include "app/melee/Game.hpp"

#include "engine/core/Types.hpp"
#include "game/Melee.hpp"
#include "game/Ships.hpp"
#include "platform/Platform.hpp"

#include <cassert>
#include <cstdio>
#include <filesystem>
#include <string_view>
#include <utility>

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
				"  Pass it explicitly:  sc2m-melee <path-to>/sc2/content\n"
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

	BattleConfig cfg;

	// Today's fixed roster, by catalog key. What ships exist and what they
	// load is game/Ships.cpp's business; only the match-up is decided here.
	cfg.roster = {game::findShip("earthling.cruiser"),
			game::findShip("ilwrath.avenger")};
	assert(cfg.roster[0] != nullptr && cfg.roster[1] != nullptr);

	// Warm the cache and say what failed, now rather than mid-battle: a
	// missing id would otherwise first be reported the frame something tries
	// to draw it. Addressed by resource id, not path -- uqm.rmp is the link.
	const auto warm = [&](std::string_view id) {
		const game::SpriteSet &set = g.content.sprites(g.window, id);
		if (!set.valid() && g.content.valid())
			std::fprintf(stderr, "content: could not load %.*s\n",
					static_cast<int>(id.size()), id.data());
	};
	const auto warmSounds = [&](std::string_view id) {
		const std::span<const platform::Sound> set =
				g.content.sounds(g.audio, id);
		usize ok = 0;
		for (const platform::Sound &snd : set)
			ok += snd.valid() ? 1 : 0;
		std::fprintf(stderr, "audio: %.*s -> %zu/%zu loaded\n",
				static_cast<int>(id.size()), id.data(), ok, set.size());
	};

	for (const game::ShipDef *def : cfg.roster)
	{
		warm(def->art.ship);
		warm(def->art.weapon);
		warmSounds(def->art.sounds);
	}
	warm(game::kMeleeArt.asteroid);
	warm(game::kMeleeArt.blast);
	warm(game::kMeleeArt.boom);
	warm(game::kMeleeArt.planet);
	warm(game::kMeleeArt.stars);
	warmSounds(game::kMeleeArt.battleSounds);
	if (!g.audio.valid())
		std::fprintf(stderr, "audio: no device; the game runs silent\n");

	// Skipping this leaves a default ShipSpec with thrust.max = 0 and
	// turnWait = 0, a ship that cannot accelerate and spins every frame.
	for (usize p = 0; p < cfg.roster.size(); ++p)
	{
		const game::ShipDef &def = *cfg.roster[p];
		cfg.shipData[p] = game::materialize(def, g.content, g.window);

		if (!cfg.shipData[p].valid())
		{
			std::fprintf(stderr,
					"ship: %.*s's descriptor was never filled in -- it will "
					"not fly. This is a setup bug, not a control one.\n",
					static_cast<int>(def.key.size()), def.key.data());
		}
	}

	g.battle.setContext<BattleConfig>(std::move(cfg));
}

}  // namespace uqm::melee
