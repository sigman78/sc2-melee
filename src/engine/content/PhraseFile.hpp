// Copyright the Ur-Quan Masters contributors. GPL-2.0-or-later.

#ifndef UQM2_ENGINE_CONTENT_PHRASEFILE_HPP
#define UQM2_ENGINE_CONTENT_PHRASEFILE_HPP

#include "ContentError.hpp"

#include <cassert>
#include <cstddef>
#include <string_view>
#include <vector>

namespace uqm::content {

// LIFETIME: every view points into the .txt (and .ts) text, which must
// outlive the file. 5,034 phrases x 4 strings was a lot of malloc to answer
// "what is phrase 12 called" (docs/cpp-conventions.md rules 1 and 5).
struct Phrase
{
	std::string_view name;        // the #(NAME) label
	std::string_view text;        // body, trailing blank lines trimmed
	std::string_view clip;        // optional voice file on the header line
	std::string_view timestamps;  // from the .ts; empty when there is none
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
// tools/check-phrases.py exists and why byOrdinal() is kept: it is the bridge
// between the two, and the fixtures in tests/fixtures/comm are name-keyed
// precisely so they survive the switch.
class PhraseFile
{
public:
	[[nodiscard]] std::size_t size() const noexcept { return phrases_.size(); }
	[[nodiscard]] bool empty() const noexcept { return phrases_.empty(); }

	[[nodiscard]] auto begin() const noexcept { return phrases_.begin(); }
	[[nodiscard]] auto end() const noexcept { return phrases_.end(); }

	[[nodiscard]] const Phrase &operator[](std::size_t i) const noexcept
	{
		assert(i < phrases_.size() && "phrase index out of range");
		return phrases_[i];
	}

	// nullptr when no phrase carries that name. Linear: a race file holds
	// ~130 phrases, so an index would cost more to build than it saves.
	[[nodiscard]] const Phrase *byName(std::string_view name) const noexcept;

	// 1-based, matching the enum: ordinal 1 is phrase 0. Returns nullptr for
	// 0, which the C reserves as "say nothing".
	[[nodiscard]] const Phrase *byOrdinal(std::size_t ordinal) const noexcept;

	friend PhraseFile parsePhrases(
			std::string_view, std::vector<ContentError> *);
	friend bool attachTimestamps(
			PhraseFile &, std::string_view, std::vector<ContentError> *);

private:
	std::vector<Phrase> phrases_;
};

[[nodiscard]] PhraseFile parsePhrases(
		std::string_view text, std::vector<ContentError> *problems = nullptr);

// Replays getstr.c:306-349: one .ts line per phrase, matched by strstr,
// all-or-nothing like the C (a short file discards every timestamp for the
// race). `problems` also flags a substring mismatch the C stores silently.
[[nodiscard]] bool attachTimestamps(PhraseFile &file, std::string_view ts,
		std::vector<ContentError> *problems = nullptr);

}  // namespace uqm::content

#endif  // UQM2_ENGINE_CONTENT_PHRASEFILE_HPP
