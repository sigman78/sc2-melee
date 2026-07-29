// Copyright the Ur-Quan Masters contributors. GPL-2.0-or-later.

#ifndef UQM2_ENGINE_CONTENT_BINARYTABLE_HPP
#define UQM2_ENGINE_CONTENT_BINARYTABLE_HPP

#include "Bytes.hpp"
#include "ContentError.hpp"

#include "engine/core/Types.hpp"

#include <expected>
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
// LIFETIME: entries are views into the buffer handed to parseBinaryTable,
// which must outlive the table. No content byte is copied; the one allocation
// is the vector of spans.
//
// This is only the container. What an entry *means* is recorded nowhere in
// the file -- see ColorTable.hpp, which is where that bites.
class BinaryTable
{
public:
	[[nodiscard]] usize size() const noexcept { return entries_.size(); }
	[[nodiscard]] bool empty() const noexcept { return entries_.empty(); }

	[[nodiscard]] Bytes operator[](usize i) const noexcept
	{
		assert(i < entries_.size() && "binary table index out of range");
		return entries_[i];
	}

	[[nodiscard]] auto begin() const noexcept { return entries_.begin(); }
	[[nodiscard]] auto end() const noexcept { return entries_.end(); }

	friend std::expected<BinaryTable, ContentError> parseBinaryTable(Bytes);

private:
	std::vector<Bytes> entries_;
};

[[nodiscard]] std::expected<BinaryTable, ContentError> parseBinaryTable(
		Bytes bytes);

}  // namespace uqm::content

#endif  // UQM2_ENGINE_CONTENT_BINARYTABLE_HPP
