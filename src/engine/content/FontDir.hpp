// Copyright the Ur-Quan Masters contributors. GPL-2.0-or-later.

#ifndef UQM2_ENGINE_CONTENT_FONTDIR_HPP
#define UQM2_ENGINE_CONTENT_FONTDIR_HPP

#include "ContentError.hpp"

#include <cstdint>
#include <filesystem>
#include <vector>

namespace uqm::content {

struct Glyph
{
	char32_t codepoint = 0;
	std::filesystem::path file;
};

// A .fon is a *directory*, not a file -- 30 of them in the tree holding 2,599
// PNGs between them. Each member is named for the code point it draws, in
// hex: 00020.png is space, 00021.png is '!'.
//
// gfxload.c:432 parses the name with sscanf("%x.") and skips anything that
// does not parse or is above 0xFFFF, so stray files are ignored rather than
// fatal. That leniency is kept -- a README in a font directory should not
// break the game -- but the skipped names are reported, because a glyph
// silently missing from a font is the kind of thing nobody notices until a
// specific alien says a specific word.
struct Font
{
	std::vector<Glyph> glyphs;  // sorted by code point
};

// ContentError::at is the code point when there is one, 0 otherwise. Paths
// are owned here rather than viewed: they come from the directory iterator,
// not from a buffer the caller holds.
[[nodiscard]] Font loadFontDir(const std::filesystem::path &dir,
		std::vector<ContentError> *problems = nullptr);

}  // namespace uqm::content

#endif  // UQM2_ENGINE_CONTENT_FONTDIR_HPP
