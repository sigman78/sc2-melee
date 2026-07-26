// Copyright the Ur-Quan Masters contributors. GPL-2.0-or-later.

#ifndef UQM2_ENGINE_CONTENT_ANIFILE_HPP
#define UQM2_ENGINE_CONTENT_ANIFILE_HPP

#include <string>
#include <string_view>
#include <vector>

namespace uqm::content {

// How a cel's transparency is decided (gfxload.c:54-75). The C encodes all
// four cases in one int, which is why a plain `int transparentColour` reads
// as a colour index when two of its values are not one.
enum class Transparency
{
	None,          // -1: no transparency at all
	PaletteIndex,  // >= 0: that index is transparent
	BlackIsClear,  // 0 on a truecolour image: RGB 0,0,0 becomes transparent
	PngAlpha,      // -2: use the PNG tRNS chunk
};

struct Cel
{
	std::string file;              // relative to the .ani's own directory
	Transparency transparency = Transparency::None;
	int transparentIndex = -1;     // meaningful when transparency is
								   // PaletteIndex or BlackIsClear
	int colormapIndex = -1;        // a colormap slot -- see ColorTable.hpp
	int hotspotX = 0;
	int hotspotY = 0;
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
// This parser rejects a malformed line instead, and says which one.
struct AniFile
{
	std::vector<Cel> cels;
};

// `problems` collects per-line complaints. Unlike the C, a bad line is not
// silently absorbed: it is reported and skipped.
AniFile parseAni (std::string_view text, std::vector<std::string> &problems);

}  // namespace uqm::content

#endif  // UQM2_ENGINE_CONTENT_ANIFILE_HPP
