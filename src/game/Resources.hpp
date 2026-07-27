// Copyright the Ur-Quan Masters contributors. GPL-2.0-or-later.

#ifndef UQM2_GAME_RESOURCES_HPP
#define UQM2_GAME_RESOURCES_HPP

#include "engine/content/ResourceMap.hpp"
#include "game/SpriteSet.hpp"
#include "platform/Audio.hpp"
#include "platform/Platform.hpp"

#include <filesystem>
#include <map>
#include <string>
#include <span>
#include <string_view>
#include <vector>

namespace uqm::game {

// Content, addressed by resource id, not path: uqm.rmp is the only
// name<->file link (blackur/ loads comm.kohrah.*, not matching by
// directory), and it's what an addon overrides instead of a hardcoded path.
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

	// Loads and caches the sprites a GFXRES id names. On failure the set is
	// the placeholder: no frames -- the caller draws its Rect fallback --
	// but one block mask, so nothing spawns maskless and no caller carries
	// a stand-in of its own. Cached by id: every asteroid in the field
	// shares one upload.
	[[nodiscard]] const SpriteSet &sprites(
			platform::Platform &window, std::string_view id);

	// The sounds a SNDRES id names: a .snd lists .wav filenames one per
	// line, like a .ani lists cels, so sounds arrive as an indexed set the
	// caller picks by slot. Cached by id, like sprites.
	[[nodiscard]] std::span<const platform::Sound> sounds(
			const platform::Audio &audio, std::string_view id);

private:
	std::filesystem::path root_;
	std::string text_;                 // owns what map_ views
	content::ResourceMap map_;
	std::map<std::string, SpriteSet, std::less<>> sprites_;
	std::map<std::string, std::vector<platform::Sound>, std::less<>> sounds_;
};

}  // namespace uqm::game

#endif  // UQM2_GAME_RESOURCES_HPP
