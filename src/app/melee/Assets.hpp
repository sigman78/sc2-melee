// Copyright the Ur-Quan Masters contributors. GPL-2.0-or-later.

#ifndef UQM2_APP_MELEE_ASSETS_HPP
#define UQM2_APP_MELEE_ASSETS_HPP

#include <filesystem>

namespace uqm::melee {

struct Game;

// Content lives beside the executable or in the working directory,
// searched upward from each so running out of build/release/src needs no
// argument; a directory counts only once it has uqm.rmp in it.
[[nodiscard]] std::filesystem::path findContent(
		const std::filesystem::path &override_);

// Opens content, uploads every sprite and sound set the melee needs, and
// wires the ship descriptors' weapon masks up to the loaded projectile art.
void loadAssets(Game &g, const std::filesystem::path &content);

}  // namespace uqm::melee

#endif  // UQM2_APP_MELEE_ASSETS_HPP
