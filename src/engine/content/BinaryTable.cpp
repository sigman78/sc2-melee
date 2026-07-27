// Copyright the Ur-Quan Masters contributors. GPL-2.0-or-later.

#include "BinaryTable.hpp"

#include "engine/core/Types.hpp"

namespace uqm::content {

namespace {
constexpr u32 kUncompressed = 0xFFFFFFFFu;
constexpr usize kPrefixSize = 4;
constexpr usize kHeaderSize = 8;  // count + extra
}  // namespace

std::expected<BinaryTable, ContentError>
parseBinaryTable(Bytes bytes)
{
	using enum ContentErrorCode;

	if (bytes.empty())
		return std::unexpected(ContentError{Empty});
	if (bytes.size() < kPrefixSize + kHeaderSize)
	{
		return std::unexpected(ContentError{
				TooShort, 0, kPrefixSize + kHeaderSize, bytes.size()});
	}

	// The original stored an LZ-compressed length here; loadres.c:36-40
	// refuses those outright and so do we -- no shipped content uses it.
	if (readU32BE(bytes, 0) != kUncompressed)
		return std::unexpected(ContentError{Compressed});

	const Bytes data = bytes.subspan(kPrefixSize);
	const auto count = readU32BE(data, 0);
	const auto extra = readU32BE(data, 4);

	// count and extra are file data, so every use of them is checked against
	// the buffer before it sizes anything.
	if (!fits(data, kHeaderSize, static_cast<usize>(count) * 4))
	{
		return std::unexpected(
				ContentError{CountOverflows, 0, count, data.size()});
	}

	const usize headerWords = usize{2} + count + static_cast<usize>(extra);
	if (headerWords > data.size() / 4)
	{
		return std::unexpected(
				ContentError{CountOverflows, 0, headerWords * 4, data.size()});
	}

	BinaryTable table;
	table.entries_.reserve(count);

	usize at = headerWords * 4;
	for (u32 i = 0; i < count; ++i)
	{
		const auto len =
				readU32BE(data, kHeaderSize + usize{i} * 4);
		if (!fits(data, at, len))
		{
			return std::unexpected(
					ContentError{EntryOverruns, i, len, data.size() - at});
		}
		table.entries_.push_back(data.subspan(at, len));
		at += len;
	}

	// Every .ct in the tree consumes exactly to the end. Slack would mean the
	// format has a case this does not model, which is worth hearing about
	// rather than tolerating.
	if (at != data.size())
	{
		return std::unexpected(ContentError{TrailingBytes,
				static_cast<u32>(at), data.size(), at});
	}

	return table;
}

}  // namespace uqm::content
