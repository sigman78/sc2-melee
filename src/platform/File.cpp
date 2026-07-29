// Copyright the Ur-Quan Masters contributors. GPL-2.0-or-later.

#include "File.hpp"

#include "engine/core/Types.hpp"

#include <array>
#include <cassert>
#include <limits>
#include <utility>

namespace uqm::platform {

namespace {

// Indexed by FileError. A constexpr array of views, not a function full of
// string literals and not a std::string in sight
// (docs/cpp-conventions.md rule 1).
constexpr std::array<std::string_view, 6> kFileErrorText{
		"file not found",
		"not a regular file",
		"could not be opened",
		"read failed",
		"write failed",
		"file is too large to load",
};

}  // namespace

std::string_view describe(FileError e) noexcept
{
	const auto i = static_cast<usize>(e);
	assert(i < kFileErrorText.size());
	return kFileErrorText[i];
}

File::~File()
{
	close();
}

File::File(File &&other) noexcept : fp_(std::exchange(other.fp_, nullptr)) {}

File &File::operator=(File &&other) noexcept
{
	if (this != &other)
	{
		close();
		fp_ = std::exchange(other.fp_, nullptr);
	}
	return *this;
}

void File::close() noexcept
{
	if (fp_ != nullptr)
	{
		std::fclose(fp_);
		fp_ = nullptr;
	}
}

std::expected<File, FileError> File::open(
		const std::filesystem::path &path, Mode mode)
{
#ifdef _WIN32
	// The narrow CRT entry point would mangle any non-ASCII path through the
	// active code page; content directories are user-chosen, so take the
	// wide one.
	std::FILE *fp = _wfopen(path.c_str(), mode == Mode::Read ? L"rb" : L"wb");
#else
	std::FILE *fp = std::fopen(path.c_str(), mode == Mode::Read ? "rb" : "wb");
#endif
	if (fp == nullptr)
		return std::unexpected(FileError::OpenFailed);
	return File(fp);
}

std::expected<usize, FileError> File::read(std::span<std::byte> into)
{
	assert(isOpen() && "read from a closed File");
	if (into.empty())
		return usize{0};

	const usize got = std::fread(into.data(), 1, into.size(), fp_);
	if (got != into.size() && std::ferror(fp_) != 0)
		return std::unexpected(FileError::ReadFailed);
	return got;
}

std::expected<usize, FileError> File::write(std::span<const std::byte> from)
{
	assert(isOpen() && "write to a closed File");
	if (from.empty())
		return usize{0};

	const usize put = std::fwrite(from.data(), 1, from.size(), fp_);
	if (put != from.size())
		return std::unexpected(FileError::WriteFailed);
	return put;
}

namespace {

// Size from the directory entry, not by seeking: one query instead of two,
// it leaves the stream position alone, and it avoids ftell's 32-bit `long`
// on Windows. The buffer is then allocated once, at the right size.
std::expected<std::uintmax_t, FileError> sizeOnDisk(
		const std::filesystem::path &path)
{
	std::error_code ec;
	const auto status = std::filesystem::status(path, ec);
	if (ec || status.type() == std::filesystem::file_type::not_found)
		return std::unexpected(FileError::NotFound);
	if (status.type() != std::filesystem::file_type::regular)
		return std::unexpected(FileError::NotARegularFile);

	const std::uintmax_t bytes = std::filesystem::file_size(path, ec);
	if (ec)
		return std::unexpected(FileError::ReadFailed);
	if (bytes > std::numeric_limits<usize>::max() / 2)
		return std::unexpected(FileError::TooLarge);
	return bytes;
}

}  // namespace

std::expected<std::span<const std::byte>, FileError> readFileInto(
		const std::filesystem::path &path, std::vector<std::byte> &buffer)
{
	const auto bytes = sizeOnDisk(path);
	if (!bytes)
		return std::unexpected(bytes.error());

	auto file = File::open(path, File::Mode::Read);
	if (!file)
		return std::unexpected(file.error());

	buffer.resize(static_cast<usize>(*bytes));
	const auto got = file->read(buffer);
	if (!got)
		return std::unexpected(got.error());

	// A file that shrank between the stat and the read is not corruption,
	// just a race; report what is actually there.
	return std::span<const std::byte>(buffer.data(), *got);
}

std::expected<std::vector<std::byte>, FileError> readFile(
		const std::filesystem::path &path)
{
	std::vector<std::byte> buffer;
	const auto view = readFileInto(path, buffer);
	if (!view)
		return std::unexpected(view.error());
	buffer.resize(view->size());
	return buffer;
}

std::expected<void, FileError> writeFile(
		const std::filesystem::path &path, std::span<const std::byte> bytes)
{
	auto file = File::open(path, File::Mode::Write);
	if (!file)
		return std::unexpected(file.error());
	const auto put = file->write(bytes);
	if (!put)
		return std::unexpected(put.error());
	return {};
}

}  // namespace uqm::platform
