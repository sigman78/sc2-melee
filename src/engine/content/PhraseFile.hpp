// Copyright the Ur-Quan Masters contributors. GPL-2.0-or-later.

#ifndef UQM2_ENGINE_CONTENT_PHRASEFILE_HPP
#define UQM2_ENGINE_CONTENT_PHRASEFILE_HPP

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace uqm::content {

struct Phrase
{
	std::string name;   // the #(NAME) label
	std::string text;   // body, trailing blank lines trimmed
	std::string clip;   // optional voice file named on the header line
	std::string timestamps;  // from the .ts, empty when there is none
};

// A conversation .txt (getstr.c:262-360):
//
//     #(NEUTRAL_SPACE_HELLO_1)\tsupox-000.ogg
//     Greetings Fellow Carbon Creature...
//
// A #(NAME) line opens a phrase; everything up to the next one is its body.
//
// The rewrite binds phrases by name. The C binds by ordinal --
// NPCPhrase(X) reaches SetAbsStringTableIndex(table, X - 1) -- which is why
// tools/check-phrases.py exists and why `index` below is kept: it is the
// bridge between the two, and the fixtures in tests/fixtures/comm are
// name-keyed precisely so they survive the switch.
struct PhraseFile
{
	std::vector<Phrase> phrases;

	// nullopt when no phrase carries that name.
	[[nodiscard]] std::optional<std::size_t> indexOf (
			std::string_view name) const;

	// 1-based, matching the enum: ordinal 1 is phrases[0]. Returns nullptr
	// for 0, which the C reserves as "say nothing".
	[[nodiscard]] const Phrase *byOrdinal (std::size_t ordinal) const;
};

PhraseFile parsePhrases (std::string_view text,
		std::vector<std::string> &problems);

// Attach timestamps from a .ts, replaying getstr.c:306-349 exactly: one line
// per phrase, in order, matched with strstr.
//
// Returns false when the C would have given up -- and when it gives up it
// discards *every* timestamp for the race behind one log warning, so this
// returns all-or-nothing too rather than leaving a half-populated file.
// `problems` also records the silent case: strstr is a substring test, so a
// line naming FOO_EXTRA satisfies phrase FOO, and the C neither notices nor
// complains, it just stores the wrong text.
bool attachTimestamps (PhraseFile &file, std::string_view ts,
		std::vector<std::string> &problems);

}  // namespace uqm::content

#endif  // UQM2_ENGINE_CONTENT_PHRASEFILE_HPP
