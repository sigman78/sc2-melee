// Copyright the Ur-Quan Masters contributors. GPL-2.0-or-later.

#include "Resources.hpp"

#include "platform/File.hpp"

#include <utility>

namespace uqm::game {

namespace fs = std::filesystem;

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

	const auto [it, inserted] = sprites_.emplace(std::string(id), std::move(set));
	(void)inserted;
	return it->second;
}

}  // namespace uqm::game
