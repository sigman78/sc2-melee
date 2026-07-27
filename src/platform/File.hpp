// Copyright the Ur-Quan Masters contributors. GPL-2.0-or-later.

#ifndef UQM2_PLATFORM_FILE_HPP
#define UQM2_PLATFORM_FILE_HPP

#include "engine/core/Types.hpp"

#include <cstddef>
#include <cstdio>
#include <expected>
#include <filesystem>
#include <span>
#include <string_view>
#include <vector>

namespace uqm::platform {

// File access over <filesystem> for paths and the C API for bytes
// (docs/cpp-conventions.md rule 4). Not <fstream>: that drags in locales,
// per-character virtual dispatch and an exception-configurable state machine
// to do what the OS does in one call, and `istreambuf_iterator` into a
// `std::string` reads a file one virtual call at a time and then copies the
// whole thing again.

enum class FileError : u8
{
	NotFound,
	NotARegularFile,
	OpenFailed,
	ReadFailed,
	WriteFailed,
	TooLarge,
};

[[nodiscard]] std::string_view describe(FileError e) noexcept;

// RAII over FILE*. Move-only: two owners closing one handle is not a thing
// to make representable.
class File
{
public:
	File() = default;
	~File();

	File(const File &) = delete;
	File &operator=(const File &) = delete;
	File(File &&other) noexcept;
	File &operator=(File &&other) noexcept;

	enum class Mode
	{
		Read,
		Write,
	};

	[[nodiscard]] static std::expected<File, FileError> open(
			const std::filesystem::path &path, Mode mode);

	[[nodiscard]] bool isOpen() const noexcept { return fp_ != nullptr; }

	// Bytes actually transferred, or the error. A short read is not an error
	// here; callers that require exactness check the count.
	[[nodiscard]] std::expected<usize, FileError> read(
			std::span<std::byte> into);
	[[nodiscard]] std::expected<usize, FileError> write(
			std::span<const std::byte> from);

	void close() noexcept;

private:
	explicit File(std::FILE *fp) noexcept : fp_(fp) {}

	std::FILE *fp_ = nullptr;
};

// Whole-file read. One allocation, sized from the directory entry, so the
// buffer is never grown.
[[nodiscard]] std::expected<std::vector<std::byte>, FileError> readFile(
		const std::filesystem::path &path);

// Reads into a caller-owned buffer, reusing its capacity. For sweeping
// thousands of files, which is what the content tests and the browser do.
[[nodiscard]] std::expected<std::span<const std::byte>, FileError> readFileInto(
		const std::filesystem::path &path, std::vector<std::byte> &buffer);

[[nodiscard]] std::expected<void, FileError> writeFile(
		const std::filesystem::path &path, std::span<const std::byte> bytes);

// Same read, viewed as text. The bytes are not copied or transcoded; content
// text is UTF-8 and stays that way.
[[nodiscard]] inline std::string_view asText(
		std::span<const std::byte> bytes) noexcept
{
	return std::string_view(
			reinterpret_cast<const char *>(bytes.data()), bytes.size());
}

}  // namespace uqm::platform

#endif  // UQM2_PLATFORM_FILE_HPP
