#include "mnc/settings/settings.hpp"

#include <glaze/glaze.hpp>

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <regex>
#include <ranges>
#include <sstream>
#include <stdexcept>
#include <system_error>
#include <set>

#include <fcntl.h>
#include <sys/stat.h>
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
	const int descriptor = ::open(path.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
	if (descriptor < 0)
		throw std::runtime_error("cannot open directory " + path.string() +
			": " + std::strerror(errno));
	if (::fsync(descriptor) != 0) {
		const auto message = std::strerror(errno);
		::close(descriptor);
		throw std::runtime_error("cannot sync directory " + path.string() +
			": " + message);
	}
	::close(descriptor);
}

std::vector<std::string> lines(std::string_view source)
{
	std::vector<std::string> result;
	std::istringstream stream{std::string(source)};
	for (std::string line; std::getline(stream, line);)
		result.push_back(std::move(line));
	return result;
}

std::string json_text(const glz::generic_sorted *value)
{
	if (value == nullptr)
		return "<missing>";
	const auto encoded = value->dump();
	if (!encoded)
		throw std::runtime_error("cannot serialize JSON value while diffing");
	return *encoded;
}

void compare_json(const glz::generic_sorted *before,
		  const glz::generic_sorted *after, std::string path,
		  std::vector<StructuredDifference> &changes)
{
	if (before == nullptr || after == nullptr) {
		changes.push_back({std::move(path), json_text(before), json_text(after)});
		return;
	}
	if (before->is_object() && after->is_object()) {
		const auto &old_object = before->get_object();
		const auto &new_object = after->get_object();
		std::set<std::string, std::less<>> keys;
		for (const auto &[key, value] : old_object) {
			(void)value;
			keys.insert(key);
		}
		for (const auto &[key, value] : new_object) {
			(void)value;
			keys.insert(key);
		}
		for (const auto &key : keys) {
			const auto old_entry = old_object.find(key);
			const auto new_entry = new_object.find(key);
			compare_json(old_entry == old_object.end() ? nullptr : &old_entry->second,
				new_entry == new_object.end() ? nullptr : &new_entry->second,
				path + "." + key, changes);
		}
		return;
	}
	if (before->is_array() && after->is_array()) {
		const auto &old_array = before->get_array();
		const auto &new_array = after->get_array();
		const auto size = std::max(old_array.size(), new_array.size());
		for (std::size_t index = 0; index < size; ++index)
			compare_json(index < old_array.size() ? &old_array[index] : nullptr,
				index < new_array.size() ? &new_array[index] : nullptr,
				path + "[" + std::to_string(index) + "]", changes);
		return;
	}
	const auto old_text = json_text(before);
	const auto new_text = json_text(after);
	if (old_text != new_text)
		changes.push_back({std::move(path), old_text, new_text});
}

} // namespace

void AtomicFileWriter::write(const std::filesystem::path &path,
			     std::string_view contents, unsigned mode)
{
	const auto parent = path.parent_path();
	if (!parent.empty())
		std::filesystem::create_directories(parent);
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
		if (::fchmod(descriptor, mode) != 0 && errno != EPERM &&
		    errno != EOPNOTSUPP && errno != ENOTSUP)
			throw std::runtime_error("cannot set mode on " + temporary + ": " +
				std::strerror(errno));
		/* vfat and similar persistent-media filesystems derive permissions from
		 * mount options and reject fchmod even for the file creator.  The open
		 * mode remains effective on Unix filesystems; on mount-policy filesystems
		 * the image's mount owner/group/mask is the access-control boundary. */
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
		sync_directory(parent.empty() ? std::filesystem::path{"."} : parent);
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
	std::filesystem::create_directories(revisions_path());
	/* Directory ownership and access policy belong to the service manager and
	 * image integration layer.  In particular, removable/persistent product
	 * data may be backed by a filesystem such as vfat that deliberately exposes
	 * access through mount options and cannot implement chmod(2).  Requiring a
	 * chmod here made an otherwise writable settings store fatal at startup.
	 * AtomicFileWriter still requests the narrow mode for files on filesystems
	 * that support Unix permissions. */
}

std::optional<std::string> FileSettingsRepository::read_active() const
{
	return std::filesystem::exists(active_path())
		? std::optional<std::string>{read_file(active_path())} : std::nullopt;
}

std::optional<std::string> FileSettingsRepository::read_draft() const
{
	return std::filesystem::exists(draft_path())
		? std::optional<std::string>{read_file(draft_path())} : std::nullopt;
}

std::optional<std::string> FileSettingsRepository::read_pending() const
{
	return std::filesystem::exists(pending_path())
		? std::optional<std::string>{read_file(pending_path())} : std::nullopt;
}

void FileSettingsRepository::write_active(std::string_view json) const
{
	AtomicFileWriter::write(active_path(), json);
}

void FileSettingsRepository::write_draft(std::string_view json) const
{
	AtomicFileWriter::write(draft_path(), json);
}

void FileSettingsRepository::write_pending(std::string_view json) const
{
	AtomicFileWriter::write(pending_path(), json);
}

void FileSettingsRepository::remove_draft() const { remove_if_present(draft_path()); }
void FileSettingsRepository::remove_pending() const { remove_if_present(pending_path()); }
const std::filesystem::path &FileSettingsRepository::root() const noexcept { return root_; }
std::filesystem::path FileSettingsRepository::active_path() const { return root_ / "active.json"; }
std::filesystem::path FileSettingsRepository::draft_path() const { return root_ / "draft.json"; }
std::filesystem::path FileSettingsRepository::pending_path() const { return root_ / "pending.json"; }
std::filesystem::path FileSettingsRepository::revisions_path() const { return root_ / "revisions"; }
std::filesystem::path FileSettingsRepository::secrets_path() const { return root_ / "secrets.json"; }

RevisionStore::RevisionStore(std::filesystem::path directory)
	: directory_(std::move(directory))
{
}

RevisionFile RevisionStore::create(std::uint64_t revision, std::string_view hash,
				   std::string_view canonical_json) const
{
	std::filesystem::create_directories(directory_);
	char number[17]{};
	std::snprintf(number, sizeof(number), "%016llu",
		static_cast<unsigned long long>(revision));
	const auto path = directory_ /
		(std::string(number) + "-" + std::string(hash.substr(0, 12)) + ".json");
	AtomicFileWriter::write(path, canonical_json, 0440);
	/* Do not publish a revision merely because rename(2) succeeded. Persistent
	 * media faults have previously left a zero-length snapshot whose filename
	 * looked valid and incorrectly advanced the active revision after reboot. */
	if (read_file(path) != canonical_json) {
		std::error_code ignored;
		std::filesystem::remove(path, ignored);
		throw std::runtime_error("settings revision readback mismatch: " +
			path.string());
	}
	return {revision, std::string(hash), path};
}

RevisionFile RevisionStore::reset(std::string_view hash,
				  std::string_view canonical_json) const
{
	const auto parent = directory_.parent_path();
	std::filesystem::create_directories(parent);
	const auto suffix = std::to_string(static_cast<unsigned long>(::getpid()));
	const auto staging = parent / (directory_.filename().string() + ".new." + suffix);
	const auto backup = parent / (directory_.filename().string() + ".old." + suffix);
	std::error_code ignored;
	std::filesystem::remove_all(staging, ignored);
	std::filesystem::remove_all(backup, ignored);

	RevisionStore staged(staging);
	auto result = staged.create(1u, hash, canonical_json);
	bool old_moved = false;
	try {
		if (std::filesystem::exists(directory_)) {
			std::filesystem::rename(directory_, backup);
			old_moved = true;
		}
		std::filesystem::rename(staging, directory_);
		sync_directory(parent);
		std::filesystem::remove_all(backup);
		sync_directory(parent);
		result.path = directory_ / result.path.filename();
		return result;
	} catch (...) {
		std::filesystem::remove_all(staging, ignored);
		if (old_moved && !std::filesystem::exists(directory_))
			std::filesystem::rename(backup, directory_, ignored);
		throw;
	}
}

std::vector<RevisionFile> RevisionStore::list() const
{
	std::vector<RevisionFile> result;
	if (!std::filesystem::exists(directory_))
		return result;
	const std::regex pattern{"^([0-9]{16})-([0-9a-f]{12})\\.json$"};
	for (const auto &entry : std::filesystem::directory_iterator(directory_)) {
		if (!entry.is_regular_file())
			continue;
		std::error_code size_error;
		const auto size = entry.file_size(size_error);
		if (size_error || size == 0u)
			continue;
		std::smatch match;
		const auto name = entry.path().filename().string();
		if (!std::regex_match(name, match, pattern))
			continue;
		result.push_back({std::stoull(match[1].str()), match[2].str(),
			entry.path()});
	}
	std::ranges::sort(result, {}, &RevisionFile::revision);
	return result;
}

std::string RevisionStore::read(std::uint64_t revision) const
{
	for (const auto &entry : list())
		if (entry.revision == revision)
			return read_file(entry.path);
	throw std::out_of_range("settings revision does not exist");
}

void RevisionStore::erase(const RevisionFile &revision) const
{
	remove_if_present(revision.path);
	sync_directory(directory_);
}

void RevisionStore::prune(std::size_t retain) const
{
	auto entries = list();
	while (entries.size() > retain) {
		std::filesystem::remove(entries.front().path);
		entries.erase(entries.begin());
	}
}

void RevisionStore::clear() const
{
	for (const auto &entry : list())
		std::filesystem::remove(entry.path);
}

DiffResult JsonDiff::compare(std::string_view before, std::string_view after)
{
	DiffResult result;
	if (before == after)
		return result;
	glz::generic_sorted old_document;
	glz::generic_sorted new_document;
	if (const auto error = glz::read_json(old_document, before))
		throw std::runtime_error("cannot diff invalid active JSON: " +
			glz::format_error(error, before));
	if (const auto error = glz::read_json(new_document, after))
		throw std::runtime_error("cannot diff invalid draft JSON: " +
			glz::format_error(error, after));
	compare_json(&old_document, &new_document, "$", result.changes);

	const auto old_lines = lines(before);
	const auto new_lines = lines(after);
	result.unified = "--- active.json\n+++ draft.json\n";
	const auto count = std::max(old_lines.size(), new_lines.size());
	for (std::size_t index = 0; index < count; ++index) {
		const std::string old_line = index < old_lines.size() ? old_lines[index] : "";
		const std::string new_line = index < new_lines.size() ? new_lines[index] : "";
		if (old_line == new_line)
			continue;
		if (!old_line.empty())
			result.unified += "-" + old_line + "\n";
		if (!new_line.empty())
			result.unified += "+" + new_line + "\n";
	}
	return result;
}

bool SettingsChangeSet::affects(std::string_view prefix) const noexcept
{
	return std::ranges::any_of(diff.changes, [prefix](const auto &change) {
		return std::string_view(change.path).starts_with(prefix);
	});
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

bool SecretStore::exists() const { return std::filesystem::exists(path_); }
void SecretStore::clear() const { remove_if_present(path_); }

} // namespace mnc::settings
