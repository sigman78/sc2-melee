// Copyright the Ur-Quan Masters contributors. GPL-2.0-or-later.

#include "ResourceMap.hpp"

namespace uqm::content {

namespace {

std::string_view
trim(std::string_view s)
{
	const auto isSpace = [] (char c) {
		return c == ' ' || c == '\t' || c == '\r' || c == '\n';
	};
	while (!s.empty() && isSpace(s.front()))
		s.remove_prefix(1);
	while (!s.empty() && isSpace(s.back()))
		s.remove_suffix(1);
	return s;
}

}  // namespace

ResourceMap
ResourceMap::parse(std::string_view text, std::vector<std::string> &problems)
{
	ResourceMap map;
	std::size_t lineNo = 0;

	while (!text.empty())
	{
		++lineNo;
		const std::size_t nl = text.find('\n');
		std::string_view line = text.substr(0, nl);
		text = (nl == std::string_view::npos) ? std::string_view{}
											  : text.substr(nl + 1);

		line = trim(line);
		if (line.empty() || line.front() == '#')
			continue;

		const std::size_t eq = line.find('=');
		if (eq == std::string_view::npos)
		{
			problems.emplace_back("line " + std::to_string(lineNo)
					+ ": no '=' in " + std::string(line));
			continue;
		}

		const std::string_view key = trim(line.substr(0, eq));
		const std::string_view value = trim(line.substr(eq + 1));

		// TYPE:path. The colon is required -- a value without one names no
		// loader, and silently treating it as a bare path is how a typo turns
		// into a missing texture three months later.
		const std::size_t colon = value.find(':');
		if (colon == std::string_view::npos)
		{
			problems.emplace_back("line " + std::to_string(lineNo) + ": "
					+ std::string(key) + " has no TYPE: prefix");
			continue;
		}

		Resource res;
		res.type = std::string(trim(value.substr(0, colon)));
		res.path = std::string(trim(value.substr(colon + 1)));
		if (res.path.empty())
		{
			problems.emplace_back("line " + std::to_string(lineNo) + ": "
					+ std::string(key) + " has an empty path");
			continue;
		}

		auto [it, inserted] = map.entries_.emplace(std::string(key), res);
		if (!inserted)
		{
			problems.emplace_back("line " + std::to_string(lineNo)
					+ ": duplicate key " + std::string(key));
		}
	}

	return map;
}

const Resource *
ResourceMap::find(std::string_view key) const
{
	const auto it = entries_.find(key);
	return it == entries_.end() ? nullptr : &it->second;
}

}  // namespace uqm::content
