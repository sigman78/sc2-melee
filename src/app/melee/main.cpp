// Copyright the Ur-Quan Masters contributors. GPL-2.0-or-later.
//
// The M1 vertical slice: Human Cruiser against Ilwrath Avenger, two players
// at one keyboard, on a real window. The loop is `main` + `iterate()`:
// Emscripten cannot own the outer while-loop, since it must return to the
// browser between frames, so this shape works for both.

#include "app/melee/Assets.hpp"
#include "app/melee/Game.hpp"

#include <filesystem>

#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#endif

using namespace uqm::melee;

namespace {

Game *g_game = nullptr;

void iterateOnce()
{
	iterate(*g_game);
}

}  // namespace

int main(int argc, char **argv)
{
	Game game;
	setUp(game,
			findContent(argc > 1 ? std::filesystem::path(argv[1])
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
