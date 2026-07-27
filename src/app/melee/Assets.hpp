// Copyright the Ur-Quan Masters contributors. GPL-2.0-or-later.

#ifndef UQM2_APP_MELEE_ASSETS_HPP
#define UQM2_APP_MELEE_ASSETS_HPP

#include <filesystem>

namespace uqm::melee {

struct Game;

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
[[nodiscard]] std::filesystem::path findContent(
		const std::filesystem::path &override_);

// Opens content, uploads every sprite and sound set the melee needs, and
// wires the ship descriptors' weapon masks up to the loaded projectile art.
void loadAssets(Game &g, const std::filesystem::path &content);

}  // namespace uqm::melee

#endif  // UQM2_APP_MELEE_ASSETS_HPP
