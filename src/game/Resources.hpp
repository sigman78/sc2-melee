// Copyright the Ur-Quan Masters contributors. GPL-2.0-or-later.

#ifndef UQM2_GAME_RESOURCES_HPP
#define UQM2_GAME_RESOURCES_HPP

#include "engine/content/ResourceMap.hpp"
#include "game/SpriteSet.hpp"
#include "platform/Platform.hpp"

#include <filesystem>
#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace uqm::game {

// Content, addressed by resource id rather than by path.
//
// uqm.rmp is the only link between a name and a file, and the indirection is
// load-bearing rather than decorative:
//
//   - directory names and resource names do not agree. blackur/ loads
//     comm.kohrah.*, slyland/ loads comm.probe.*. Convention cannot shortcut
//     this because there is no convention to follow.
//   - it is what an addon overrides. Replacing a ship's art means shipping a
//     different uqm.rmp entry, not shipping a file at a path the game happens
//     to hardcode. Hardcoding paths silently removes that ability, and the
//     removal is invisible until someone tries to use it.
//
// The melee app hardcoded paths for a while and worked, which is exactly why
// this is worth writing down: it works right up until it has to not.
//
// LIFETIME: this owns the text uqm.rmp was parsed from, because every key,
// type and path in ResourceMap is a view into it.
class Resources
{
public:
	// Empty if the map could not be read; `problems` collects the reasons.
	[[nodiscard]] static Resources open(std::filesystem::path root,
			std::vector<content::ContentError> *problems = nullptr);

	[[nodiscard]] bool valid() const noexcept { return !map_.empty(); }
	[[nodiscard]] const std::filesystem::path &root() const noexcept
	{
		return root_;
	}

	// The absolute path a resource id names, or empty if the id is unknown or
	// does not name a file (`ship.supox.code = SHIP:16` names a table index,
	// not a path).
	[[nodiscard]] std::filesystem::path pathOf(std::string_view id) const;

	// Loads and caches the sprites a GFXRES id names. Returns an empty set the
	// caller can still draw nothing from, rather than failing the frame.
	//
	// Cached by id, so two ships naming the same art upload it once -- which
	// they do: every asteroid in the field shares one set.
	[[nodiscard]] const SpriteSet &sprites(
			platform::Platform &window, std::string_view id);

private:
	std::filesystem::path root_;
	std::string text_;                 // owns what map_ views
	content::ResourceMap map_;
	std::map<std::string, SpriteSet, std::less<>> sprites_;
};

}  // namespace uqm::game

#endif  // UQM2_GAME_RESOURCES_HPP
