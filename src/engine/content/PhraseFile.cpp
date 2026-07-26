// Copyright the Ur-Quan Masters contributors. GPL-2.0-or-later.

#include "PhraseFile.hpp"

namespace uqm::content {

namespace {

std::string_view
trimRight(std::string_view s)
{
	while (!s.empty()
			&& (s.back() == '\r' || s.back() == '\n' || s.back() == ' '
					|| s.back() == '\t'))
		s.remove_suffix(1);
	return s;
}

std::string_view
nextLine(std::string_view &text)
{
	const std::size_t nl = text.find('\n');
	const std::string_view line = text.substr(0, nl);
	text = (nl == std::string_view::npos) ? std::string_view{}
										  : text.substr(nl + 1);
	return line;
}

// "#(NAME)\tclip.ogg" -> name and clip. nullopt when the line is not a header.
struct Header
{
	std::string_view name;
	std::string_view clip;
};

// One strtok step: skip leading delimiters, then take everything up to the
// next one. Returns nullopt for "no token left", which is what strtok's NULL
// means and which the caller has to distinguish from "empty token".
std::optional<std::string_view>
strtokStep(std::string_view &rest, std::string_view delims)
{
	while (!rest.empty() && delims.find(rest.front()) != std::string_view::npos)
		rest.remove_prefix(1);
	if (rest.empty())
		return std::nullopt;

	const std::size_t end = rest.find_first_of(delims);
	const std::string_view token = rest.substr(0, end);
	rest = (end == std::string_view::npos) ? std::string_view{}
										   : rest.substr(end + 1);
	return token;
}

// getstr.c:523-546, reproduced rather than approximated. The exact strtok
// behaviour matters more than it looks:
//
//   "#(NAME)\tclip.ogg"  -> name "NAME", clip "clip.ogg"
//   "#()"                -> no token at all; strtok skips both parens and
//                           runs off the end, so `if (s)` fails and the line
//                           is skipped *entirely* -- not appended as body
//                           text either, because the `else if` is on the
//                           other arm of the same `if (line[0] == '#')`.
//                           base/gamestrings.txt relies on this; treating
//                           these as empty-named phrases would shift every
//                           ordinal after them.
//   "# comment"          -> name " comment". A '#' line with no parens is a
//                           phrase header with a silly name, not a comment.
//                           There are no comments in this format.
std::optional<Header>
parseHeader(std::string_view line)
{
	if (line.empty() || line.front() != '#')
		return std::nullopt;

	std::string_view rest = line.substr(1);
	const auto name = strtokStep(rest, "()");
	if (!name)
		return std::nullopt;

	Header h;
	h.name = *name;
	// The C's second strtok, with a different delimiter set.
	h.clip = strtokStep(rest, " \t\r\n)").value_or(std::string_view{});
	return h;
}

}  // namespace

std::optional<std::size_t>
PhraseFile::indexOf(std::string_view name) const
{
	for (std::size_t i = 0; i < phrases.size(); ++i)
	{
		if (phrases[i].name == name)
			return i;
	}
	return std::nullopt;
}

const Phrase *
PhraseFile::byOrdinal(std::size_t ordinal) const
{
	if (ordinal == 0 || ordinal > phrases.size())
		return nullptr;
	return &phrases[ordinal - 1];
}

PhraseFile
parsePhrases(std::string_view text, std::vector<std::string> &problems)
{
	PhraseFile file;
	std::size_t lineNo = 0;
	std::string body;
	bool open = false;

	const auto flush = [&] () {
		if (!open)
			return;
		// getstr.c:284-292 walks back over trailing newlines.
		std::string_view trimmed = trimRight(body);
		file.phrases.back().text = std::string(trimmed);
		body.clear();
	};

	while (!text.empty())
	{
		++lineNo;
		const std::string_view line = nextLine(text);

		// The C branches on line[0] == '#' first and only then asks whether a
		// name came out of it. A '#' line that yields no name is dropped on
		// the floor -- it never reaches the append branch. Mirroring that
		// structure, rather than just the successful case, is what keeps
		// "#()" from becoming body text.
		if (!line.empty() && line.front() == '#')
		{
			const auto header = parseHeader(line);
			if (!header)
				continue;

			flush();
			Phrase p;
			p.name = std::string(header->name);
			p.clip = std::string(header->clip);
			file.phrases.push_back(std::move(p));
			open = true;
		}
		else if (open)
		{
			body.append(trimRight(line));
			body.push_back('\n');
		}
		else if (!trimRight(line).empty())
		{
			problems.emplace_back("line " + std::to_string(lineNo)
					+ ": text before the first #(NAME); the C drops it");
		}
	}
	flush();

	return file;
}

bool
attachTimestamps(PhraseFile &file, std::string_view ts,
		std::vector<std::string> &problems)
{
	std::vector<std::string> collected;
	collected.reserve(file.phrases.size());

	std::string_view rest = ts;
	for (std::size_t i = 0; i < file.phrases.size(); ++i)
	{
		if (rest.empty())
		{
			problems.emplace_back("timestamps run out at phrase "
					+ std::to_string(i + 1) + " ("
					+ file.phrases[i].name
					+ "); the C disables every timestamp for this race");
			return false;
		}

		const std::string_view line = trimRight(nextLine(rest));
		const std::string &name = file.phrases[i].name;

		if (line.empty() || line.front() != '#')
		{
			problems.emplace_back("timestamp line " + std::to_string(i + 1)
					+ " is not a #(NAME) line; the C disables every timestamp "
					  "for this race");
			return false;
		}

		// strstr, exactly as the C does it.
		const std::size_t at = line.find(name);
		if (at == std::string_view::npos)
		{
			problems.emplace_back("timestamp line " + std::to_string(i + 1)
					+ " does not mention " + name
					+ "; the C disables every timestamp for this race");
			return false;
		}

		// The substring match the C never notices. It finds the name inside a
		// longer one, takes everything after it as the timing data, and
		// stores garbage without a warning.
		if (const auto header = parseHeader(line))
		{
			if (header->name != name)
			{
				problems.emplace_back("timestamp line "
						+ std::to_string(i + 1) + " names "
						+ std::string(header->name) + " but phrase "
						+ std::to_string(i + 1) + " is " + name
						+ "; strstr matches anyway, so the C stores garbage "
						  "timings silently");
			}
		}

		std::string_view data = line.substr(at + name.size());
		while (!data.empty()
				&& (data.front() == ' ' || data.front() == '\t'
						|| data.front() == ')'))
			data.remove_prefix(1);
		collected.emplace_back(data);
	}

	for (std::size_t i = 0; i < file.phrases.size(); ++i)
		file.phrases[i].timestamps = collected[i];
	return true;
}

}  // namespace uqm::content
