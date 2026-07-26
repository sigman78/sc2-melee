// Copyright the Ur-Quan Masters contributors. GPL-2.0-or-later.

#include "BinaryTable.hpp"

namespace uqm::content {

namespace {
constexpr std::uint32_t kUncompressed = 0xFFFFFFFFu;
constexpr std::size_t kPrefixSize = 4;
}  // namespace

std::optional<BinaryTable>
parseBinaryTable(Bytes bytes, std::string &error)
{
	if (bytes.size() < kPrefixSize + 8)
	{
		error = "too short to be a binary table";
		return std::nullopt;
	}

	const std::uint32_t prefix = readU32BE(bytes, 0);
	if (prefix != kUncompressed)
	{
		// The original stored an LZ-compressed length here. loadres.c:36-40
		// refuses those outright and so do we -- no shipped content uses it.
		error = "LZ-compressed resource data is not supported (length prefix "
				"is not ~0)";
		return std::nullopt;
	}

	const Bytes data = bytes.subspan(kPrefixSize);
	const std::uint32_t count = readU32BE(data, 0);
	const std::uint32_t extra = readU32BE(data, 4);

	// count is attacker-controlled in the sense that it is file data: check it
	// against the buffer before using it to size anything.
	if (!fits(data, 8, static_cast<std::size_t>(count) * 4))
	{
		error = "entry count " + std::to_string(count)
				+ " does not fit in " + std::to_string(data.size())
				+ " bytes";
		return std::nullopt;
	}

	const std::size_t headerWords =
			std::size_t{2} + count + static_cast<std::size_t>(extra);
	if (headerWords > data.size() / 4)
	{
		error = "header of " + std::to_string(headerWords)
				+ " words overruns the file";
		return std::nullopt;
	}

	BinaryTable table;
	table.entries.reserve(count);

	std::size_t at = headerWords * 4;
	for (std::uint32_t i = 0; i < count; ++i)
	{
		const std::uint32_t len = readU32BE(data, 8 + std::size_t{i} * 4);
		if (!fits(data, at, len))
		{
			error = "entry " + std::to_string(i) + " of "
					+ std::to_string(len) + " bytes overruns the file at "
					+ std::to_string(at);
			return std::nullopt;
		}
		table.entries.push_back(data.subspan(at, len));
		at += len;
	}

	// Every .ct in the tree consumes exactly to the end. Slack would mean the
	// format has a case this does not model, which is worth hearing about
	// rather than tolerating.
	if (at != data.size())
	{
		error = "trailing " + std::to_string(data.size() - at)
				+ " bytes after the last entry";
		return std::nullopt;
	}

	return table;
}

}  // namespace uqm::content
