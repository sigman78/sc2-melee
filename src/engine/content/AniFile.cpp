// Copyright the Ur-Quan Masters contributors. GPL-2.0-or-later.

#include "AniFile.hpp"

#include "engine/core/Text.hpp"
#include "engine/core/Types.hpp"

#include <array>

namespace uqm::content {

AniFile parseAni(std::string_view text, std::vector<ContentError> *problems)
{
	using enum ContentErrorCode;

	AniFile ani;

	const auto note = [problems](ContentErrorCode code, usize line,
							  u64 expected = 0, u64 actual = 0) {
		if (problems != nullptr)
		{
			problems->emplace_back(
					code, static_cast<u32>(line), expected, actual);
		}
	};

	forEachLine(text, [&](std::string_view line, usize lineNo) {
		// Five fields, in order. Collected into a fixed array rather than a
		// vector: the count is part of the format, so a sixth field is an
		// error, not a resize.
		std::array<std::string_view, 5> field{};
		usize count = 0;
		forEachField(line, [&](std::string_view f) {
			if (count < field.size())
				field[count] = f;
			++count;
		});

		if (count == 0)
		{
			// The C counts this as a cel and then reuses the previous
			// filename. Say so instead.
			note(BadFieldCount, lineNo, field.size(), 0);
			return;
		}
		if (count != field.size())
		{
			note(BadFieldCount, lineNo, field.size(), count);
			return;
		}

		Cel cel;
		cel.file = field[0];

		i32 transparent = 0;
		if (!parseInt(field[1], transparent)
				|| !parseInt(field[2], cel.colormapIndex)
				|| !parseInt(field[3], cel.hotspot.x)
				|| !parseInt(field[4], cel.hotspot.y))
		{
			note(NonNumericField, lineNo);
			return;
		}

		// gfxload.c:54-75. -1 and -2 are sentinels, not indices; 0 means
		// "index 0" on a paletted image and "black is clear" on a truecolour
		// one, which the loader cannot tell apart until the PNG is open.
		switch (transparent)
		{
		case -1:
			cel.transparency = Transparency::None;
			break;
		case -2:
			cel.transparency = Transparency::PngAlpha;
			break;
		case 0:
			cel.transparency = Transparency::BlackIsClear;
			cel.transparentIndex = 0;
			break;
		default:
			if (transparent < 0)
			{
				note(BadTransparency, lineNo, 0, static_cast<u64>(transparent));
				return;
			}
			cel.transparency = Transparency::PaletteIndex;
			cel.transparentIndex = transparent;
			break;
		}

		ani.cels.push_back(cel);
	});

	return ani;
}

}  // namespace uqm::content
