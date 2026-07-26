// Copyright the Ur-Quan Masters contributors. GPL-2.0-or-later.

#ifndef UQM2_ENGINE_CONTENT_RESOURCEMAP_HPP
#define UQM2_ENGINE_CONTENT_RESOURCEMAP_HPP

#include <cstddef>
#include <functional>
#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace uqm::content {

// uqm.rmp: `key = TYPE:path`, e.g.
//
//     comm.supox.dialogue = CONVERSATION:base/comm/supox/supox.txt
//
// The map is the only link between a C identifier and a file. Source
// directory names and content directory names do not match -- blackur/ loads
// comm.kohrah.*, slyland/ loads comm.probe.* -- so this indirection is not
// decoration and cannot be shortcut by convention.
struct Resource
{
	std::string type;  // STRTAB, BINTAB, CONVERSATION, GFXRES, FONTRES, ...
	std::string path;  // relative to the content root
};

class ResourceMap
{
public:
	// Parses the text of a .rmp. Unparseable lines are collected in
	// `problems` rather than dropped, so a browser can show them; a map with
	// problems is still usable for the keys that did parse.
	static ResourceMap parse(std::string_view text,
			std::vector<std::string> &problems);

	[[nodiscard]] const Resource *find(std::string_view key) const;
	[[nodiscard]] std::size_t size() const { return entries_.size(); }

	[[nodiscard]] const std::map<std::string, Resource, std::less<>> &
	entries() const
	{
		return entries_;
	}

private:
	std::map<std::string, Resource, std::less<>> entries_;
};

}  // namespace uqm::content

#endif  // UQM2_ENGINE_CONTENT_RESOURCEMAP_HPP
