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

// A .fon is a *directory* of PNGs named by code point in hex (00020.png is
// space). gfxload.c:432 parses names with sscanf("%x."); ones that don't
// match are skipped, not fatal, but reported -- a missing glyph should not
// be silent.
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
