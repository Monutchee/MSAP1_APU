#include "msap1/waveform/mncwf_v4_export.hpp"

#include <cerrno>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <string>
#include <system_error>
#include <utility>

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

namespace msap1 {
namespace {

class FileDescriptor final {
public:
	explicit FileDescriptor(int descriptor = -1) : value_(descriptor) {}
	~FileDescriptor()
	{
		if (value_ >= 0)
			::close(value_);
	}
	FileDescriptor(const FileDescriptor &) = delete;
	FileDescriptor &operator=(const FileDescriptor &) = delete;
	FileDescriptor(FileDescriptor &&other) noexcept
		: value_(std::exchange(other.value_, -1))
	{
	}
	FileDescriptor &operator=(FileDescriptor &&other) noexcept
	{
		if (this == &other)
			return *this;
		if (value_ >= 0)
			::close(value_);
		value_ = std::exchange(other.value_, -1);
		return *this;
	}
	[[nodiscard]] int get() const noexcept { return value_; }

private:
	int value_;
};

[[noreturn]] void system_failure(std::string_view operation)
{
	throw std::system_error(errno, std::generic_category(),
		std::string(operation));
}

void validate_file_name(std::string_view name)
{
	if (name.empty() || name == "." || name == ".." ||
	    name.find('/') != std::string_view::npos ||
	    name.find('\\') != std::string_view::npos ||
	    !name.ends_with(".mncwf"))
		throw std::invalid_argument("invalid MNCWF capture file name");
}

MncwfValidationMode export_validation_mode(
	std::span<const std::byte> bytes) noexcept
{
	if (bytes.size() < 12u)
		return MncwfValidationMode::complete;
	std::uint32_t version = 0u;
	for (unsigned index = 0; index < 4u; ++index)
		version |= static_cast<std::uint32_t>(
			std::to_integer<std::uint8_t>(bytes[8u + index])) <<
			(index * 8u);
	return version == mncwf_v5_version
		? MncwfValidationMode::metadata_only
		: MncwfValidationMode::complete;
}

class MappedFile final {
public:
	MappedFile(const std::filesystem::path &directory, std::string_view file_name)
	{
		validate_file_name(file_name);
		FileDescriptor directory_fd(::open(directory.c_str(),
			O_RDONLY | O_CLOEXEC | O_DIRECTORY));
		if (directory_fd.get() < 0)
			system_failure("open waveform directory");
		std::string owned_name(file_name);
		file_ = FileDescriptor(::openat(directory_fd.get(), owned_name.c_str(),
			O_RDONLY | O_CLOEXEC | O_NOFOLLOW));
		if (file_.get() < 0)
			system_failure("open MNCWF capture");

		struct stat status {};
		if (::fstat(file_.get(), &status) != 0)
			system_failure("inspect MNCWF capture");
		if (!S_ISREG(status.st_mode) || status.st_size <= 0)
			throw std::invalid_argument(
				"MNCWF capture is not a non-empty regular file");
		const auto unsigned_size = static_cast<std::uint64_t>(status.st_size);
		if (unsigned_size > mncwf_v4_max_file_bytes ||
		    unsigned_size > std::numeric_limits<std::size_t>::max())
			throw std::invalid_argument("MNCWF capture exceeds the size bound");
		size_ = static_cast<std::size_t>(unsigned_size);
		mapping_ = ::mmap(nullptr, size_, PROT_READ, MAP_PRIVATE,
			file_.get(), 0);
		if (mapping_ == MAP_FAILED) {
			mapping_ = nullptr;
			system_failure("map MNCWF capture");
		}
	}

	~MappedFile()
	{
		if (mapping_ != nullptr)
			::munmap(mapping_, size_);
	}
	MappedFile(const MappedFile &) = delete;
	MappedFile &operator=(const MappedFile &) = delete;

	[[nodiscard]] std::span<const std::byte> bytes() const noexcept
	{
		return {static_cast<const std::byte *>(mapping_), size_};
	}

private:
	FileDescriptor file_;
	void *mapping_ = nullptr;
	std::size_t size_ = 0;
};

} // namespace

struct MncwfV4ExportFile::Impl {
	Impl(const std::filesystem::path &directory, std::string_view file_name,
		const MncwfUuid &event_uuid)
		: mapping(directory, file_name),
		  reader(mapping.bytes(), export_validation_mode(mapping.bytes())),
		  virtual_file(make_mncwf_v4_event_slice(reader, event_uuid))
	{
	}

	MappedFile mapping;
	MncwfV4Reader reader;
	MncwfV4VirtualFile virtual_file;
};

std::shared_ptr<MncwfV4ExportFile> MncwfV4ExportFile::open(
	const std::filesystem::path &directory, std::string_view file_name,
	const MncwfUuid &event_uuid)
{
	return std::shared_ptr<MncwfV4ExportFile>(new MncwfV4ExportFile(
		std::make_unique<Impl>(directory, file_name, event_uuid)));
}

MncwfV4ExportFile::MncwfV4ExportFile(
	std::unique_ptr<Impl> implementation)
	: impl_(std::move(implementation))
{
}

MncwfV4ExportFile::~MncwfV4ExportFile() = default;

std::uint64_t MncwfV4ExportFile::size() const noexcept
{
	return impl_->virtual_file.size();
}

const MncwfUuid &MncwfV4ExportFile::capture_uuid() const noexcept
{
	return impl_->virtual_file.capture_uuid();
}

std::uint64_t MncwfV4ExportFile::first_sequence() const noexcept
{
	return impl_->virtual_file.first_sequence();
}

std::uint64_t MncwfV4ExportFile::last_sequence() const noexcept
{
	return impl_->virtual_file.last_sequence();
}

std::size_t MncwfV4ExportFile::read(std::uint64_t offset,
	std::span<std::byte> destination) const noexcept
{
	return impl_->virtual_file.read(offset, destination);
}

} // namespace msap1
