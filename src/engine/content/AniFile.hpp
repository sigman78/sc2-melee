// Copyright the Ur-Quan Masters contributors. GPL-2.0-or-later.

#ifndef UQM2_ENGINE_CONTENT_ANIFILE_HPP
#define UQM2_ENGINE_CONTENT_ANIFILE_HPP

#include "ContentError.hpp"
#include "engine/core/Geometry.hpp"
#include "engine/core/Types.hpp"

#include <string_view>
#include <vector>

namespace uqm::content {

// How a cel's transparency is decided (gfxload.c:54-75). The C packs all four
// cases into one int, which is why a plain `int transparentColour` reads as a
// colour index when two of its values are not one.
enum class Transparency : u8
{
	None,          // -1: no transparency at all
	PaletteIndex,  // >= 0: that index is transparent
	BlackIsClear,  // 0 on a truecolour image: RGB 0,0,0 becomes transparent
	PngAlpha,      // -2: use the PNG tRNS chunk
};

struct Cel
{
	// LIFETIME: a view into the .ani text, which must outlive the AniFile.
	std::string_view file;

	Transparency transparency = Transparency::None;
	i32 transparentIndex = -1;  // when PaletteIndex or BlackIsClear
	i32 colormapIndex = -1;     // a colormap slot; see ColorTable.hpp
	Vec2i hotspot;
};

// A sprite index: CRLF text, one line per cel,
//
//     supox-000.png -1 10 0 0
//     <file> <transparentColour> <colormapIndex> <hotspotX> <hotspotY>
//
// See docs/content-formats.md. All 584 files in the tree are CRLF and every
// line has exactly five fields, so the C's sscanf happens to be safe -- but
// its cel count is a *line* count (gfxload.c:223-228) while the second pass
// only advances on a successful image load, so a blank line would leave the
// filename buffer holding the previous line and silently duplicate a cel.
// This parser rejects the line instead, and says which one.
struct AniFile
{
	std::vector<Cel> cels;
};

// `problems` is optional and stays empty in the normal case, so a caller that
// does not want diagnostics allocates nothing for them. ContentError::at is
// the 1-based line number.
[[nodiscard]] AniFile parseAni(
		std::string_view text, std::vector<ContentError> *problems = nullptr);

}  // namespace uqm::content

#endif  // UQM2_ENGINE_CONTENT_ANIFILE_HPP
