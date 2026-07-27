// Copyright the Ur-Quan Masters contributors. GPL-2.0-or-later.

#include "Resources.hpp"

#include "engine/core/Text.hpp"
#include "platform/File.hpp"

#include <cstddef>
#include <cstdint>
#include <utility>

namespace uqm::game {

namespace fs = std::filesystem;

namespace {

// The stand-in for art that did not load: no frames, so the draw side's
// Rect fallback stays deliberately ugly, but one solid block mask, so a
// maskless element cannot exist and no caller carries its own fallback.
// One size for everything -- the block stands in for a silhouette, it does
// not try to be one; per-kind sizing died with the Game's mask members
// (review-003 R4).
[[nodiscard]] SpriteSet
placeholder()
{
	constexpr std::uint32_t kSide = 12;
	const std::vector<std::uint8_t> bits(
			static_cast<std::size_t>(kSide) * kSide, 1);
	SpriteSet set;
	set.masks.emplace_back(Extent2u{kSide, kSide},
			Vec2i{static_cast<std::int32_t>(kSide / 2),
				static_cast<std::int32_t>(kSide / 2)},
			bits);
	return set;
}

}  // namespace

Resources
Resources::open(fs::path root, std::vector<content::ContentError> *problems)
{
	Resources r;
	r.root_ = std::move(root);

	const auto bytes = platform::readFile(r.root_ / "uqm.rmp");
	if (!bytes)
		return r;  // invalid(): the caller reports, since it knows the context

	// Copied, not viewed. ResourceMap holds string_views into this text for as
	// long as it lives, so the bytes have to outlive the parse -- and the
	// buffer readFile hands back does not.
	const std::string_view view = platform::asText(*bytes);
	r.text_.assign(view.begin(), view.end());
	r.map_ = content::ResourceMap::parse(r.text_, problems);
	return r;
}

fs::path
Resources::pathOf(std::string_view id) const
{
	const content::Resource *res = map_.find(id);
	if (res == nullptr || !res->isPath())
		return {};
	return root_ / fs::path(std::string(res->path));
}

const SpriteSet &
Resources::sprites(platform::Platform &window, std::string_view id)
{
	if (const auto it = sprites_.find(id); it != sprites_.end())
		return it->second;

	SpriteSet set;
	const fs::path path = pathOf(id);
	if (!path.empty())
	{
		// colortable.main is the global palette table ship cels index into.
		// Looked up by id like everything else rather than assumed to be at
		// base/uqm.ct, which is the whole point of this class.
		set = loadSprites(window, path, pathOf("colortable.main"));
	}

	// The invariant every consumer leans on: a set from here is either the
	// art or the placeholder -- never frameless *and* maskless.
	if (!set.valid())
		set = placeholder();

	const auto [it, inserted] = sprites_.emplace(std::string(id), std::move(set));
	(void)inserted;
	return it->second;
}

std::span<const platform::Sound>
Resources::sounds(const platform::Audio &audio, std::string_view id)
{
	if (const auto it = sounds_.find(id); it != sounds_.end())
		return it->second;

	std::vector<platform::Sound> loaded;
	const fs::path list = pathOf(id);
	if (!list.empty())
	{
		if (const auto text = platform::readFile(list); text)
		{
			// One filename per line, relative to the .snd's own directory.
			// Blank lines are skipped rather than treated as a missing file,
			// because trailing newlines are ordinary.
			const fs::path dir = list.parent_path();
			forEachLine(platform::asText(*text),
					[&](std::string_view line, std::size_t) {
				const std::string_view name = trim(line);
				if (name.empty())
					return;
				platform::Sound s = audio.load(dir / std::string(name));
				loaded.push_back(std::move(s));
			});
		}
	}

	const auto [it, inserted] = sounds_.emplace(std::string(id), std::move(loaded));
	(void)inserted;
	return it->second;
}

}  // namespace uqm::game
