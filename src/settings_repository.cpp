#include "mnc/settings/settings.hpp"

#include <cerrno>
#include <cstring>
#include <fstream>
#include <stdexcept>
#include <system_error>
#include <utility>

#include <fcntl.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <unistd.h>

namespace mnc::settings {
namespace {

std::string read_file(const std::filesystem::path &path)
{
	std::ifstream input(path, std::ios::binary);
	if (!input)
		throw std::runtime_error("cannot open " + path.string());
	return {std::istreambuf_iterator<char>(input),
		std::istreambuf_iterator<char>()};
}

void remove_if_present(const std::filesystem::path &path)
{
	std::error_code error;
	if (!std::filesystem::remove(path, error) && error)
		throw std::runtime_error("cannot remove " + path.string() + ": " +
			error.message());
}

void sync_directory(const std::filesystem::path &path)
{
	const int descriptor = ::open(path.c_str(),
		O_RDONLY | O_DIRECTORY | O_CLOEXEC);
	if (descriptor < 0)
		throw std::runtime_error("cannot open directory " + path.string() +
			": " + std::strerror(errno));
	if (::fsync(descriptor) != 0) {
		const auto message = std::strerror(errno);
		(void)::close(descriptor);
		throw std::runtime_error("cannot sync directory " + path.string() +
			": " + message);
	}
	(void)::close(descriptor);
}

} // namespace

void AtomicFileWriter::write(const std::filesystem::path &path,
			     std::string_view contents, unsigned mode)
{
	const auto parent = path.parent_path();
	if (!parent.empty())
		std::filesystem::create_directories(parent);
	const auto filesystem_path = parent.empty()
		? std::filesystem::path{"."} : parent;
	struct statvfs filesystem_status {};
	if (::statvfs(filesystem_path.c_str(), &filesystem_status) != 0)
		throw std::runtime_error("cannot inspect filesystem containing " +
			path.string() + ": " + std::strerror(errno));
	if ((filesystem_status.f_flag & ST_RDONLY) != 0)
		throw std::runtime_error("filesystem containing " + path.string() +
			" is read-only; repair the persistent-data filesystem before saving settings");

	const auto temporary = path.string() + ".tmp." +
		std::to_string(static_cast<unsigned long>(::getpid()));
	const int descriptor = ::open(temporary.c_str(),
		O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, mode);
	if (descriptor < 0)
		throw std::runtime_error("cannot create " + temporary + ": " +
			std::strerror(errno));
	bool descriptor_open = true;
	try {
		std::size_t offset = 0;
		while (offset < contents.size()) {
			const auto written = ::write(descriptor, contents.data() + offset,
				contents.size() - offset);
			if (written < 0) {
				if (errno == EINTR)
					continue;
				throw std::runtime_error("cannot write " + temporary +
					": " + std::strerror(errno));
			}
			offset += static_cast<std::size_t>(written);
		}
		if (::fchmod(descriptor, mode) != 0)
			throw std::runtime_error("cannot set mode on " + temporary +
				": " + std::strerror(errno));
		if (::fsync(descriptor) != 0)
			throw std::runtime_error("cannot sync " + temporary + ": " +
				std::strerror(errno));
		if (::close(descriptor) != 0)
			throw std::runtime_error("cannot close " + temporary + ": " +
				std::strerror(errno));
		descriptor_open = false;
		if (::rename(temporary.c_str(), path.c_str()) != 0)
			throw std::runtime_error("cannot publish " + path.string() +
				": " + std::strerror(errno));
		sync_directory(filesystem_path);
	} catch (...) {
		if (descriptor_open)
			(void)::close(descriptor);
		std::error_code ignored;
		std::filesystem::remove(temporary, ignored);
		throw;
	}
}

FileSettingsRepository::FileSettingsRepository(std::filesystem::path root)
	: root_(std::move(root))
{
}

void FileSettingsRepository::initialize_layout() const
{
	std::filesystem::create_directories(root_);
}

std::optional<std::string> FileSettingsRepository::read_active() const
{
	return std::filesystem::exists(active_path())
		? std::optional<std::string>{read_file(active_path())} : std::nullopt;
}

void FileSettingsRepository::write_active(std::string_view json) const
{
	AtomicFileWriter::write(active_path(), json);
}

const std::filesystem::path &FileSettingsRepository::root() const noexcept
{
	return root_;
}

std::filesystem::path FileSettingsRepository::active_path() const
{
	return root_ / "active.json";
}

std::filesystem::path FileSettingsRepository::secrets_path() const
{
	return root_ / "secrets.json";
}

SecretStore::SecretStore(std::filesystem::path path) : path_(std::move(path)) {}

void SecretStore::replace(std::string_view canonical_json) const
{
	AtomicFileWriter::write(path_, canonical_json, 0600);
}

std::optional<std::string> SecretStore::read_document() const
{
	return exists() ? std::optional<std::string>{read_file(path_)} : std::nullopt;
}

bool SecretStore::exists() const
{
	return std::filesystem::exists(path_);
}

void SecretStore::clear() const
{
	remove_if_present(path_);
}

} // namespace mnc::settings
