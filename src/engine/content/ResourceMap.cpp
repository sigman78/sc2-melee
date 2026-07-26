// Copyright the Ur-Quan Masters contributors. GPL-2.0-or-later.

#include "ResourceMap.hpp"

#include "engine/core/Text.hpp"

#include <algorithm>

namespace uqm::content {

ResourceMap
ResourceMap::parse(std::string_view text, std::vector<ContentError> *problems)
{
	using enum ContentErrorCode;

	ResourceMap map;
	// One allocation for the whole file: uqm.rmp has 963 entries and roughly
	// as many lines, so this is close and never grows more than once.
	map.entries_.reserve(1024);

	const auto note = [problems](ContentErrorCode code, std::size_t line) {
		if (problems != nullptr)
			problems->emplace_back(code, static_cast<std::uint32_t>(line));
	};

	forEachLine(text, [&](std::string_view raw, std::size_t lineNo) {
		const std::string_view line = trim(raw);
		if (line.empty() || line.front() == '#')
			return;

		const std::size_t eq = line.find('=');
		if (eq == std::string_view::npos)
		{
			note(BadFieldCount, lineNo);
			return;
		}

		Resource res;
		res.key = trim(line.substr(0, eq));
		const std::string_view value = trim(line.substr(eq + 1));

		// TYPE:path. The colon is required -- a value without one names no
		// loader, and silently treating it as a bare path is how a typo turns
		// into a missing texture three months later.
		const std::size_t colon = value.find(':');
		if (colon == std::string_view::npos)
		{
			note(BadFieldCount, lineNo);
			return;
		}

		res.type = trim(value.substr(0, colon));
		res.path = trim(value.substr(colon + 1));
		if (res.path.empty() || res.key.empty())
		{
			note(BadFieldCount, lineNo);
			return;
		}

		map.entries_.push_back(res);
	});

	std::ranges::sort(map.entries_, {}, &Resource::key);

	// Duplicate keys mean one of them silently wins. Which one depended on
	// std::map's insert-first-wins before; now it is at least reported.
	const auto dup = std::ranges::adjacent_find(
			map.entries_, {}, &Resource::key);
	if (dup != map.entries_.end())
		note(BadFieldCount, 0);

	return map;
}

const Resource *
ResourceMap::find(std::string_view key) const noexcept
{
	const auto it = std::ranges::lower_bound(entries_, key, {}, &Resource::key);
	if (it == entries_.end() || it->key != key)
		return nullptr;
	return &*it;
}

}  // namespace uqm::content
