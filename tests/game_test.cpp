// Copyright the Ur-Quan Masters contributors. GPL-2.0-or-later.
//
// The game's data definitions against the real content. Every resource id a
// ShipDef or the melee table names must resolve in uqm.rmp, so a key typo
// fails here rather than as a silent rectangle at runtime. Together with
// browse_inventory (rmp -> file) this closes the chain definition -> rmp ->
// file.
//
// No framework, matching the other tests: non-zero exit means failure.

#include "engine/content/ResourceMap.hpp"
#include "engine/core/Text.hpp"
#include "engine/core/Types.hpp"
#include "game/Melee.hpp"
#include "game/Ships.hpp"
#include "platform/File.hpp"

#include <cstdio>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

using namespace uqm;
namespace fs = std::filesystem;

namespace {

int failures = 0;

#define CHECK(cond, ...)                                                       \
	do                                                                         \
	{                                                                          \
		if (!(cond))                                                           \
		{                                                                      \
			std::printf("FAIL %s:%d: ", __FILE__, __LINE__);                   \
			std::printf(__VA_ARGS__);                                          \
			std::printf("\n");                                                 \
			++failures;                                                        \
		}                                                                      \
	} while (0)

void checkResolves(const content::ResourceMap &map, std::string_view id,
		std::string_view what)
{
	const content::Resource *res = map.find(id);
	CHECK(res != nullptr, "%.*s names \"%.*s\", not in uqm.rmp",
			static_cast<int>(what.size()), what.data(),
			static_cast<int>(id.size()), id.data());
}

void testCatalog(const content::ResourceMap &map)
{
	CHECK(!game::shipCatalog().empty(), "the catalog is empty");

	for (const game::ShipDef &def : game::shipCatalog())
	{
		CHECK(!def.key.empty(), "a ShipDef has no key");
		CHECK(def.spec != nullptr && def.spec->valid(),
				"%.*s: spec missing or invalid",
				static_cast<int>(def.key.size()), def.key.data());

		// The key is an identity: it must find exactly this entry.
		CHECK(game::findShip(def.key) == &def,
				"findShip(%.*s) does not round-trip",
				static_cast<int>(def.key.size()), def.key.data());

		checkResolves(map, def.art.ship, def.key);
		checkResolves(map, def.art.weapon, def.key);
		checkResolves(map, def.art.sounds, def.key);
	}

	CHECK(game::findShip("no.such.ship") == nullptr,
			"an unknown key should find nothing");
}

void testMeleeArt(const content::ResourceMap &map)
{
	checkResolves(map, game::kMeleeArt.asteroid, "melee.asteroid");
	checkResolves(map, game::kMeleeArt.blast, "melee.blast");
	checkResolves(map, game::kMeleeArt.boom, "melee.boom");
	checkResolves(map, game::kMeleeArt.planet, "melee.planet");
	checkResolves(map, game::kMeleeArt.stars, "melee.stars");
	checkResolves(map, game::kMeleeArt.battleSounds, "melee.battleSounds");
}

// BattleSound names battle.snd's lines through Damaged6Plus = 5; that is a
// claim about the content, so the content gets to veto it.
void testBattleSoundSlots(
		const content::ResourceMap &map, const fs::path &content)
{
	const content::Resource *res = map.find(game::kMeleeArt.battleSounds);
	if (res == nullptr)
		return;  // already reported by testMeleeArt

	const auto bytes = platform::readFile(content / std::string(res->path));
	CHECK(bytes.has_value(), "cannot read battle.snd");
	if (!bytes)
		return;

	usize lines = 0;
	forEachLine(platform::asText(*bytes), [&](std::string_view line, usize) {
		if (!trim(line).empty())
			++lines;
	});
	CHECK(lines > slot(game::BattleSound::Damaged6Plus),
			"battle.snd has %zu slots; BattleSound expects at least %zu", lines,
			slot(game::BattleSound::Damaged6Plus) + 1);
}

}  // namespace

int main(int argc, char **argv)
{
	if (argc < 2)
	{
		std::printf("usage: game_test <content-dir>\n");
		return 2;
	}

	const auto rmpBytes = platform::readFile(fs::path(argv[1]) / "uqm.rmp");
	CHECK(rmpBytes.has_value(), "cannot read uqm.rmp");
	if (!rmpBytes)
		return 1;

	std::vector<content::ContentError> problems;
	const content::ResourceMap map =
			content::ResourceMap::parse(platform::asText(*rmpBytes), &problems);
	CHECK(problems.empty(), "uqm.rmp had %zu unparseable lines",
			problems.size());

	testCatalog(map);
	testMeleeArt(map);
	testBattleSoundSlots(map, fs::path(argv[1]));

	if (failures != 0)
	{
		std::printf("%d failure(s)\n", failures);
		return 1;
	}
	std::printf("game_test: all passed\n");
	return 0;
}
