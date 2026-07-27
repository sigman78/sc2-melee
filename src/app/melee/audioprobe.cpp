#include "engine/core/Types.hpp"
#include "game/Resources.hpp"
#include "platform/Audio.hpp"

#include <cstdio>
#include <filesystem>

using namespace uqm;

int
main(int argc, char **argv)
{
	const std::filesystem::path root = argc > 1
			? std::filesystem::path(argv[1])
			: std::filesystem::path("D:/non-esp/sc2-uqm/sc2/content");

	platform::Audio audio;
	std::printf("device: %s\n", audio.valid() ? "open" : "UNAVAILABLE");

	game::Resources res = game::Resources::open(root);
	std::printf("resources: %s\n", res.valid() ? "ok" : "FAILED");

	for (const char *id : {"ship.earthling.sounds", "ship.ilwrath.sounds",
				 "sounds.battle"})
	{
		std::printf("  %-24s path=%s\n", id, res.pathOf(id).string().c_str());
		const auto set = res.sounds(audio, id);
		usize ok = 0;
		for (const platform::Sound &s : set)
			ok += s.valid() ? 1 : 0;
		std::printf("  %-24s %zu/%zu loaded\n", id, ok, set.size());
	}
	return 0;
}
