// Copyright the Ur-Quan Masters contributors. GPL-2.0-or-later.

#include "PhraseFile.hpp"

#include "engine/core/Text.hpp"
#include "engine/core/Types.hpp"

#include <optional>

namespace uqm::content {

namespace {

struct Header
{
	std::string_view name;
	std::string_view clip;
};

// One strtok step: skip leading delimiters, then take everything up to the
// next one. nullopt is strtok's NULL -- "no token at all" -- which the caller
// has to tell apart from "empty token".
std::optional<std::string_view>
strtokStep(std::string_view &rest, std::string_view delims) noexcept
{
	while (!rest.empty()
			&& delims.find(rest.front()) != std::string_view::npos)
		rest.remove_prefix(1);
	if (rest.empty())
		return std::nullopt;

	const usize end = rest.find_first_of(delims);
	const std::string_view token = rest.substr(0, end);
	rest = (end == std::string_view::npos) ? std::string_view{}
										   : rest.substr(end + 1);
	return token;
}

// getstr.c:523-546, reproduced exactly: "#()" yields no token, so strtok's
// NULL drops the line entirely (not even as body text) -- base/gamestrings.txt
// relies on this. A bare '#' line is a header with a silly name, not a comment.
std::optional<Header>
parseHeader(std::string_view line) noexcept
{
	if (line.empty() || line.front() != '#')
		return std::nullopt;

	std::string_view rest = line.substr(1);
	const auto name = strtokStep(rest, "()");
	if (!name)
		return std::nullopt;

	// The C's second strtok, with a different delimiter set.
	return Header{*name, strtokStep(rest, " \t\r\n)").value_or(std::string_view{})};
}

}  // namespace

const Phrase *
PhraseFile::byName(std::string_view name) const noexcept
{
	for (const Phrase &p : phrases_)
		if (p.name == name)
			return &p;
	return nullptr;
}

const Phrase *
PhraseFile::byOrdinal(usize ordinal) const noexcept
{
	if (ordinal == 0 || ordinal > phrases_.size())
		return nullptr;
	return &phrases_[ordinal - 1];
}

PhraseFile
parsePhrases(std::string_view text, std::vector<ContentError> *problems)
{
	PhraseFile file;

	// Body is a view, not a rebuilt string (trimmed per getstr.c:284-292);
	// see docs/cpp-conventions.md rule 5 for why a dropped "#()" line still
	// lets a view work here. `skippedInBody` flags the one case that would not.
	const char *bodyBegin = nullptr;
	const char *bodyEnd = nullptr;
	bool skippedInBody = false;

	const auto closeBody = [&] {
		if (!file.phrases_.empty() && bodyBegin != nullptr && bodyEnd != nullptr)
		{
			file.phrases_.back().text = std::string_view(
					bodyBegin, static_cast<usize>(bodyEnd - bodyBegin));
		}
		bodyBegin = nullptr;
		bodyEnd = nullptr;
		skippedInBody = false;
	};

	forEachLine(text, [&](std::string_view line, usize lineNo) {
		// The C branches on line[0] == '#' first and only then asks whether a
		// name came out of it. A '#' line that yields no name is dropped on
		// the floor -- it never reaches the append branch.
		if (!line.empty() && line.front() == '#')
		{
			const auto header = parseHeader(line);
			if (!header)
			{
				// A "#()" the C drops. Harmless at the end of a body, fatal
				// to a contiguous view if real text follows it.
				if (bodyBegin != nullptr)
					skippedInBody = true;
				return;
			}

			closeBody();
			file.phrases_.push_back(Phrase{header->name, {}, header->clip, {}});
			return;
		}

		if (file.phrases_.empty())
		{
			if (!trim(line).empty() && problems != nullptr)
			{
				// Text before the first #(NAME); the C drops it.
				problems->emplace_back(ContentErrorCode::BadFieldCount,
						static_cast<u32>(lineNo));
			}
			return;
		}

		if (bodyBegin == nullptr)
			bodyBegin = line.data();
		if (!trim(line).empty())
		{
			if (skippedInBody && problems != nullptr)
			{
				// Text after a dropped "#()" in the same phrase. The C
				// concatenates across it; a view cannot, so say so rather
				// than hand back a body with a stray "#()" inside it.
				problems->emplace_back(ContentErrorCode::WrongSize,
						static_cast<u32>(lineNo));
			}
			skippedInBody = false;
			bodyEnd = line.data() + line.size();
		}
	});
	closeBody();

	return file;
}

bool
attachTimestamps(PhraseFile &file, std::string_view ts,
		std::vector<ContentError> *problems)
{
	using enum ContentErrorCode;

	const auto note = [problems](ContentErrorCode code, usize at) {
		if (problems != nullptr)
			problems->emplace_back(code, static_cast<u32>(at));
	};

	// Collected first, applied only on success: the C's failure mode is to
	// discard every timestamp for the race, so a half-populated file would
	// not be reproducing it.
	std::vector<std::string_view> collected;
	collected.reserve(file.size());

	std::string_view rest = ts;
	for (usize i = 0; i < file.size(); ++i)
	{
		const std::string_view name = file[i].name;

		if (rest.empty())
		{
			note(TooShort, i + 1);
			return false;
		}

		const std::string_view line = trimRight(nextLine(rest));
		if (line.empty() || line.front() != '#')
		{
			note(BadFieldCount, i + 1);
			return false;
		}

		// strstr, exactly as the C does it.
		const usize at = line.find(name);
		if (at == std::string_view::npos)
		{
			note(NonNumericField, i + 1);
			return false;
		}

		// The substring match the C never notices: it finds the name inside a
		// longer one, takes everything after it as timing data, and stores
		// garbage without a warning.
		if (const auto header = parseHeader(line);
				header && header->name != name)
		{
			note(WrongSize, i + 1);
		}

		std::string_view data = line.substr(at + name.size());
		while (!data.empty()
				&& (data.front() == ' ' || data.front() == '\t'
						|| data.front() == ')'))
			data.remove_prefix(1);
		collected.push_back(data);
	}

	for (usize i = 0; i < file.phrases_.size(); ++i)
		file.phrases_[i].timestamps = collected[i];
	return true;
}

}  // namespace uqm::content
