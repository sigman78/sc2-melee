// Copyright the Ur-Quan Masters contributors. GPL-2.0-or-later.

#include "SpriteSet.hpp"

#include "engine/content/AniFile.hpp"
#include "engine/content/BinaryTable.hpp"
#include "engine/content/ColorTable.hpp"
#include "engine/content/PngImage.hpp"
#include "engine/content/Sprite.hpp"
#include "platform/File.hpp"

#include <cstdint>
#include <map>
#include <string>
#include <utility>

namespace uqm::game {

namespace fs = std::filesystem;

namespace {

// Slot -> palette, from a .ct read in the Palettes shape. One entry covers a
// range of slots, so one entry can populate many.
using SlotMap = std::map<int, content::Palette>;

SlotMap
loadColormaps(const fs::path &ct)
{
	SlotMap slots;

	const auto bytes = platform::readFile(ct);
	if (!bytes)
		return slots;

	const auto table = content::parseBinaryTable(*bytes);
	if (!table)
		return slots;

	for (std::size_t i = 0; i < table->size(); ++i)
	{
		const auto entry = content::parseColorTableEntry(
				(*table)[i], content::ColorTableShape::Palettes);
		if (!entry)
			continue;  // a partial-palette table; nothing a sprite can use
		for (std::size_t p = 0; p < entry->paletteCount(); ++p)
			slots[entry->range().first + static_cast<int>(p)] =
					entry->palette(p);
	}
	return slots;
}

}  // namespace

SpriteSet
loadSprites(platform::Platform &window, const fs::path &ani,
		const fs::path &colortable)
{
	SpriteSet out;

	const auto text = platform::readFile(ani);
	if (!text)
		return out;

	std::vector<content::ContentError> problems;
	const content::AniFile index =
			content::parseAni(platform::asText(*text), &problems);
	if (index.cels.empty())
		return out;

	// Ship cels name a slot in the *global* table (colortable.main =
	// base/uqm.ct); there is no .ct in any ship directory.
	//
	// Both M1 ships happen to render identically with and without it -- their
	// own PLTEs already carry the intended colours, which is why the Ilwrath
	// Avenger is legitimately a magenta-purple crescent and not, as it first
	// appears, a missing-colormap failure. The slot lookup is wired anyway,
	// because ships whose art *is* a neutral ramp exist and will need it, and
	// finding that out sprite-by-sprite later is worse than carrying it now.
	const SlotMap slots = loadColormaps(colortable);

	const fs::path dir = ani.parent_path();
	out.frames.reserve(index.cels.size());
	out.masks.reserve(index.cels.size());
	out.silhouettes.reserve(index.cels.size());

	for (const content::Cel &cel : index.cels)
	{
		const auto bytes = platform::readFile(dir / std::string(cel.file));
		if (!bytes)
			return SpriteSet{};

		const auto img = content::decodePng(*bytes);
		if (!img)
			return SpriteSet{};

		const auto slot = slots.find(cel.colormapIndex);
		const content::Palette *palette =
				slot != slots.end() ? &slot->second : nullptr;
		const std::vector<std::uint8_t> rgba = content::toRgba(*img, palette);

		platform::Texture tex = window.upload(img->size(), rgba);
		if (!tex.valid())
			return SpriteSet{};

		// The fill version: same alpha, every visible pixel white.
		std::vector<std::uint8_t> white = rgba;
		for (std::size_t i = 0; i + 3 < white.size(); i += 4)
		{
			if (white[i + 3] == 0)
				continue;
			white[i] = 255;
			white[i + 1] = 255;
			white[i + 2] = 255;
		}
		platform::Texture fill = window.upload(img->size(), white);

		// The hotspot is the .ani's, not the image centre. It is where the
		// game considers the object to *be* -- the C positions everything by
		// it, and a sprite drawn from its corner sits visibly off its own
		// collision mask.
		out.masks.emplace_back(img->size(), cel.hotspot,
				content::opacityBits(rgba, img->size()));
		out.frames.push_back(std::move(tex));
		out.silhouettes.push_back(std::move(fill));
	}

	return out;
}

}  // namespace uqm::game
