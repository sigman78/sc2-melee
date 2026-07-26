// Copyright the Ur-Quan Masters contributors. GPL-2.0-or-later.

#ifndef UQM2_ENGINE_CONTENT_RESOURCEMAP_HPP
#define UQM2_ENGINE_CONTENT_RESOURCEMAP_HPP

#include "ContentError.hpp"

#include <cstddef>
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
	std::string_view key;
	std::string_view type;  // STRTAB, BINTAB, CONVERSATION, GFXRES, ...
	std::string_view path;  // relative to the content root

	// Not every value is a path. `ship.supox.code = SHIP:16` names an index
	// into a table compiled into the binary (dummy.c:157), because a ship's
	// code is code. A caller that stats every `path` reports all 28 of them
	// as missing, which is exactly what happened.
	[[nodiscard]] bool isPath() const noexcept { return type != "SHIP"; }
};

// LIFETIME: every view points into the text passed to parse(), which must
// outlive the map.
//
// A sorted flat array rather than std::map: 963 keys meant 963 separately
// allocated nodes and two std::strings each, to answer a question that binary
// search over one contiguous allocation answers faster
// (docs/cpp-conventions.md rules 1 and 5).
class ResourceMap
{
public:
	[[nodiscard]] static ResourceMap parse(std::string_view text,
			std::vector<ContentError> *problems = nullptr);

	[[nodiscard]] const Resource *find(std::string_view key) const noexcept;

	[[nodiscard]] std::size_t size() const noexcept { return entries_.size(); }
	[[nodiscard]] bool empty() const noexcept { return entries_.empty(); }

	[[nodiscard]] auto begin() const noexcept { return entries_.begin(); }
	[[nodiscard]] auto end() const noexcept { return entries_.end(); }

private:
	std::vector<Resource> entries_;  // sorted by key
};

}  // namespace uqm::content

#endif  // UQM2_ENGINE_CONTENT_RESOURCEMAP_HPP
