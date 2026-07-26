// Copyright the Ur-Quan Masters contributors. GPL-2.0-or-later.
//
// Content library tests. Two halves, and both matter:
//
//   - unit cases over hand-built bytes, including the malformed ones, since
//     "rejects garbage with a useful message" is most of what this library is
//     for;
//   - a sweep over the whole real content tree, because the formats' surprises
//     are empirical (docs/content-formats.md) and a parser that only ever sees
//     three curated files will not meet them.
//
// No framework, matching tests/coroutine_test.c: non-zero exit means failure.

#include "engine/content/AniFile.hpp"
#include "engine/content/BinaryTable.hpp"
#include "engine/content/ColorTable.hpp"
#include "engine/content/ResourceMap.hpp"

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <string>
#include <vector>

using namespace uqm::content;
namespace fs = std::filesystem;

static int failures = 0;

#define CHECK(cond, ...)                                                      \
	do                                                                        \
	{                                                                         \
		if (!(cond))                                                          \
		{                                                                     \
			std::printf ("FAIL %s:%d: ", __FILE__, __LINE__);                 \
			std::printf (__VA_ARGS__);                                        \
			std::printf ("\n");                                               \
			++failures;                                                       \
		}                                                                     \
	} while (0)

static std::vector<std::byte>
readFile (const fs::path &p)
{
	std::ifstream in (p, std::ios::binary);
	if (!in)
		return {};
	const std::string s ((std::istreambuf_iterator<char> (in)),
			std::istreambuf_iterator<char> ());
	std::vector<std::byte> out (s.size ());
	std::memcpy (out.data (), s.data (), s.size ());
	return out;
}

static std::string
readText (const fs::path &p)
{
	std::ifstream in (p, std::ios::binary);
	if (!in)
		return {};
	return std::string ((std::istreambuf_iterator<char> (in)),
			std::istreambuf_iterator<char> ());
}

// --------------------------------------------------------------------------
// Unit cases

static void
testBinaryTableRejectsGarbage ()
{
	std::string err;

	CHECK (!parseBinaryTable ({}, err), "empty input should not parse");

	// A compressed prefix. loadres.c refuses these and so do we.
	const std::byte compressed[] = {std::byte{0}, std::byte{0}, std::byte{1},
		std::byte{0}, std::byte{0}, std::byte{0}, std::byte{0}, std::byte{0},
		std::byte{0}, std::byte{0}, std::byte{0}, std::byte{0}};
	CHECK (!parseBinaryTable (compressed, err),
			"an LZ length prefix should be refused");
	CHECK (err.find ("compressed") != std::string::npos,
			"error should mention compression, got: %s", err.c_str ());

	// count claims one entry of 16 bytes but the file holds none.
	const std::byte truncated[] = {std::byte{0xFF}, std::byte{0xFF},
		std::byte{0xFF}, std::byte{0xFF}, std::byte{0}, std::byte{0},
		std::byte{0}, std::byte{1}, std::byte{0}, std::byte{0}, std::byte{0},
		std::byte{0}, std::byte{0}, std::byte{0}, std::byte{0},
		std::byte{16}};
	CHECK (!parseBinaryTable (truncated, err),
			"an entry running past EOF should be refused");
}

static void
testColorTableShapesAreNotInterchangeable ()
{
	// A one-slot Palettes entry: [10, 10] + 768 bytes.
	std::vector<std::byte> palettes (2 + 768, std::byte{0});
	palettes[0] = std::byte{10};
	palettes[1] = std::byte{10};

	std::string err;
	const auto asPalettes =
			parseColorTableEntry (palettes, ColorTableShape::Palettes, err);
	CHECK (asPalettes.has_value (), "770-byte entry should parse as Palettes: %s",
			err.c_str ());
	if (asPalettes)
	{
		CHECK (asPalettes->palettes.size () == 1, "expected 1 palette, got %zu",
				asPalettes->palettes.size ());
		CHECK (asPalettes->first == 10 && asPalettes->last == 10,
				"slot range should be 10..10");
	}

	// The same bytes read as a partial palette must fail, not silently take
	// the first 3 bytes. This is the whole reason the shape is a parameter.
	CHECK (!parseColorTableEntry (palettes, ColorTableShape::PartialPalette,
				   err),
			"Palettes bytes must not parse as PartialPalette");

	// And the converse: a planets-style entry, [128, 255] + 128*3.
	std::vector<std::byte> partial (2 + 128 * 3, std::byte{0});
	partial[0] = std::byte{128};
	partial[1] = std::byte{255};
	const auto asPartial = parseColorTableEntry (
			partial, ColorTableShape::PartialPalette, err);
	CHECK (asPartial.has_value (), "386-byte entry should parse as partial: %s",
			err.c_str ());
	if (asPartial)
		CHECK (asPartial->colors.size () == 128, "expected 128 colours, got %zu",
				asPartial->colors.size ());
	CHECK (!parseColorTableEntry (partial, ColorTableShape::Palettes, err),
			"PartialPalette bytes must not parse as Palettes");

	// An inverted range is refused, as SetColorMap refuses it.
	std::vector<std::byte> inverted (2, std::byte{0});
	inverted[0] = std::byte{5};
	inverted[1] = std::byte{1};
	CHECK (!parseColorTableEntry (inverted, ColorTableShape::Palettes, err),
			"start > end should be refused");
}

static void
testAniRejectsWhatTheCWouldAbsorb ()
{
	std::vector<std::string> problems;

	// A blank line is the case the C turns into a duplicated cel.
	const AniFile blank = parseAni ("a.png -1 10 0 0\r\n\r\nb.png -1 10 1 2\r\n",
			problems);
	CHECK (blank.cels.size () == 2, "expected 2 cels, got %zu",
			blank.cels.size ());
	CHECK (problems.size () == 1, "expected 1 complaint about the blank line, "
								  "got %zu", problems.size ());

	problems.clear ();
	const AniFile shortLine = parseAni ("a.png -1 10\r\n", problems);
	CHECK (shortLine.cels.empty (), "a 3-field line should not yield a cel");
	CHECK (problems.size () == 1, "expected 1 complaint, got %zu",
			problems.size ());

	problems.clear ();
	const AniFile ok = parseAni ("supox-001.png -1 10 -81 -30\r\n", problems);
	CHECK (problems.empty (), "clean line should have no complaints");
	CHECK (ok.cels.size () == 1, "expected 1 cel");
	if (ok.cels.size () == 1)
	{
		CHECK (ok.cels[0].file == "supox-001.png", "filename");
		CHECK (ok.cels[0].transparency == Transparency::None,
				"-1 means no transparency");
		CHECK (ok.cels[0].colormapIndex == 10, "colormap slot");
		CHECK (ok.cels[0].hotspotX == -81 && ok.cels[0].hotspotY == -30,
				"hotspots are signed");
	}
}

// --------------------------------------------------------------------------
// The real tree

static void
sweepContent (const fs::path &content)
{
	// uqm.rmp
	std::vector<std::string> problems;
	const ResourceMap map =
			ResourceMap::parse (readText (content / "uqm.rmp"), problems);
	CHECK (problems.empty (), "uqm.rmp had %zu unparseable lines (first: %s)",
			problems.size (), problems.empty () ? "" : problems[0].c_str ());
	CHECK (map.size () > 500, "uqm.rmp should hold hundreds of keys, got %zu",
			map.size ());

	const Resource *supox = map.find ("comm.supox.dialogue");
	CHECK (supox != nullptr, "comm.supox.dialogue should be in the map");
	if (supox)
	{
		CHECK (supox->type == "CONVERSATION", "type was %s",
				supox->type.c_str ());
		CHECK (supox->path == "base/comm/supox/supox.txt", "path was %s",
				supox->path.c_str ());
	}

	// Every .ct must parse as a container, and every entry must parse as one
	// of the two shapes -- the counts are pinned so that a content change
	// that introduces a third shape fails here rather than in the renderer.
	std::size_t ctFiles = 0, shapeA = 0, shapeB = 0;
	for (const auto &e : fs::recursive_directory_iterator (content))
	{
		if (!e.is_regular_file () || e.path ().extension () != ".ct")
			continue;
		++ctFiles;

		const std::vector<std::byte> bytes = readFile (e.path ());
		std::string err;
		const auto table = parseBinaryTable (bytes, err);
		CHECK (table.has_value (), "%s: %s",
				e.path ().string ().c_str (), err.c_str ());
		if (!table)
			continue;

		for (const Bytes &entry : table->entries)
		{
			std::string errA, errB;
			if (parseColorTableEntry (entry, ColorTableShape::Palettes, errA))
				++shapeA;
			else if (parseColorTableEntry (
							 entry, ColorTableShape::PartialPalette, errB))
				++shapeB;
			else
				CHECK (false, "%s: entry of %zu bytes is neither shape (%s / %s)",
						e.path ().string ().c_str (), entry.size (),
						errA.c_str (), errB.c_str ());
		}
	}
	CHECK (ctFiles == 75, "expected 75 .ct files, found %zu", ctFiles);
	CHECK (shapeA == 79, "expected 79 full-palette entries, found %zu", shapeA);
	CHECK (shapeB == 123, "expected 123 partial-palette entries, found %zu",
			shapeB);

	// Every .ani must parse with no complaints at all.
	std::size_t aniFiles = 0, cels = 0;
	for (const auto &e : fs::recursive_directory_iterator (content))
	{
		if (!e.is_regular_file () || e.path ().extension () != ".ani")
			continue;
		++aniFiles;

		std::vector<std::string> aniProblems;
		const AniFile ani = parseAni (readText (e.path ()), aniProblems);
		CHECK (aniProblems.empty (), "%s: %s", e.path ().string ().c_str (),
				aniProblems.empty () ? "" : aniProblems[0].c_str ());
		cels += ani.cels.size ();
	}
	CHECK (aniFiles == 584, "expected 584 .ani files, found %zu", aniFiles);
	CHECK (cels > 0, "no cels parsed");

	std::printf ("swept %zu .ct (%zu+%zu entries), %zu .ani (%zu cels), "
				 "%zu resource keys\n",
			ctFiles, shapeA, shapeB, aniFiles, cels, map.size ());
}

int
main (int argc, char **argv)
{
	testBinaryTableRejectsGarbage ();
	testColorTableShapesAreNotInterchangeable ();
	testAniRejectsWhatTheCWouldAbsorb ();

	if (argc > 1)
	{
		const fs::path content = argv[1];
		if (fs::exists (content / "uqm.rmp"))
			sweepContent (content);
		else
		{
			std::printf ("FAIL no uqm.rmp under %s\n", argv[1]);
			++failures;
		}
	}
	else
	{
		std::printf ("(no content directory given; unit cases only)\n");
	}

	if (failures)
		std::printf ("%d check(s) failed\n", failures);
	return failures ? 1 : 0;
}
