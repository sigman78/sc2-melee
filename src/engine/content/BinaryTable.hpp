// Copyright the Ur-Quan Masters contributors. GPL-2.0-or-later.

#ifndef UQM2_ENGINE_CONTENT_BINARYTABLE_HPP
#define UQM2_ENGINE_CONTENT_BINARYTABLE_HPP

#include "Bytes.hpp"

#include <optional>
#include <string>
#include <vector>

namespace uqm::content {

// The binary string table that .ct (and .xlt) files are built out of.
// Specified in docs/content-formats.md; the C is loadres.c:25-53 for the
// length prefix and getstr.c:606-642 for the table itself.
//
//     u32 0xFFFFFFFF     uncompressed marker
//     u32 count
//     u32 extra          extra DWORDs before the data; 0 in all 75 files
//     u32 length[count]
//     ... entry bytes, concatenated
//
// Entries are views into the caller's buffer. Nothing is copied, and the
// buffer has to outlive the table.
//
// This is only the container. What an entry *means* is not recorded anywhere
// in the file -- see ColorTable.hpp, which is where that bites.
struct BinaryTable
{
	std::vector<Bytes> entries;
};

// Returns nullopt and sets `error` when the bytes are not a well-formed
// table. Malformed content is an expected case here, not an exception: the
// browser's job is to say which file is wrong and how.
std::optional<BinaryTable> parseBinaryTable (Bytes bytes, std::string &error);

}  // namespace uqm::content

#endif  // UQM2_ENGINE_CONTENT_BINARYTABLE_HPP
