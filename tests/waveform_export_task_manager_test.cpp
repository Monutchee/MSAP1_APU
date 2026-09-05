#include "waveform_export_task_manager.hpp"

#include <cerrno>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>

#include <fcntl.h>
#include <unistd.h>

namespace {

using msap1::web::WaveformExportStatus;
using msap1::web::WaveformExportTaskError;
using msap1::web::WaveformExportTaskManager;
using msap1::web::WaveformExportTaskOptions;

void require(bool condition, std::string_view message)
{
	if (!condition)
		throw std::runtime_error(std::string(message));
}

class TemporaryTree final {
public:
	TemporaryTree()
		: root_(std::filesystem::temp_directory_path() /
			("msap1-waveform-task-" + std::to_string(::getpid())))
	{
		std::error_code ignored;
		std::filesystem::remove_all(root_, ignored);
		std::filesystem::create_directories(source());
		std::filesystem::create_directories(exports());
	}

	~TemporaryTree()
	{
		std::error_code ignored;
		std::filesystem::remove_all(root_, ignored);
	}

	[[nodiscard]] std::filesystem::path source() const
	{
		return root_ / "source";
	}
	[[nodiscard]] std::filesystem::path exports() const
	{
		return root_ / "exports";
	}
	[[nodiscard]] std::filesystem::path root() const { return root_; }

private:
	std::filesystem::path root_;
};

void create_file(const std::filesystem::path &path, std::size_t bytes = 1u)
{
	const int descriptor = ::open(path.c_str(),
		O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
	if (descriptor < 0)
		throw std::system_error(errno, std::generic_category(),
			"create task-manager fixture");
	std::string content(bytes, '\0');
	std::size_t written = 0;
	while (written != content.size()) {
		const auto count = ::write(descriptor, content.data() + written,
			content.size() - written);
		if (count < 0 && errno == EINTR)
			continue;
		if (count <= 0) {
			const auto saved = errno;
			::close(descriptor);
			throw std::system_error(saved, std::generic_category(),
				"write task-manager fixture");
		}
		written += static_cast<std::size_t>(count);
	}
	if (::close(descriptor) != 0)
		throw std::system_error(errno, std::generic_category(),
			"close task-manager fixture");
}

template <class Function>
void require_status(Function &&function, WaveformExportStatus expected,
	std::string_view message)
{
	try {
		function();
	} catch (const WaveformExportTaskError &error) {
		require(error.status() == expected, message);
		return;
	}
	throw std::runtime_error(std::string(message));
}

WaveformExportTaskOptions options_for(const TemporaryTree &tree)
{
	WaveformExportTaskOptions options;
	options.source_root = tree.source();
	options.export_root = tree.exports();
	options.maximum_output_bytes = 1024u * 1024u;
	options.maximum_total_bytes = 2u * 1024u * 1024u;
	options.minimum_free_bytes = 0;
	options.artifact_ttl = std::chrono::seconds(2);
	options.stream_lease = std::chrono::seconds(1);
	return options;
}

void healthy_lifecycle_and_validation()
{
	TemporaryTree tree;
	create_file(tree.exports() / "orphan.part");
	create_file(tree.exports() / "orphan.cff");

	WaveformExportTaskManager manager(options_for(tree));
	const auto capabilities = manager.capabilities();
	require(capabilities.healthy && capabilities.formats.size() == 3u &&
		capabilities.maximum_output_bytes == 1024u * 1024u,
		"healthy in-process task capabilities are incomplete");
	require(std::filesystem::is_empty(tree.exports()),
		"startup did not purge orphaned export artifacts");

	require_status([&] {
		(void)manager.submit({}, "1", "capture.mncwf", "capture", {},
			"comtrade");
	}, WaveformExportStatus::invalid_request, "empty owner was accepted");
	require_status([&] {
		(void)manager.submit("admin", "0", "capture.mncwf", "capture", {},
			"comtrade");
	}, WaveformExportStatus::invalid_request, "zero session was accepted");
	require_status([&] {
		(void)manager.submit("admin", "1", "../capture.mncwf", "capture", {},
			"comtrade");
	}, WaveformExportStatus::invalid_request, "unsafe source path was accepted");
	require_status([&] {
		(void)manager.submit("admin", "1", "capture.mncwf", "capture", {},
			"unknown");
	}, WaveformExportStatus::invalid_request, "unknown format was accepted");
	require_status([&] {
		(void)manager.submit("admin", "1", "capture.mncwf", "event", {},
			"pqdif");
	}, WaveformExportStatus::invalid_request,
		"event export without event UUID was accepted");
	require_status([&] {
		(void)manager.submit("admin", "1", "missing.mncwf", "capture", {},
			"comtrade");
	}, WaveformExportStatus::not_found, "missing source did not return not-found");

	create_file(tree.source() / "invalid.mncwf", 64u);
	require_status([&] {
		(void)manager.submit("admin", "1", "invalid.mncwf", "capture", {},
			"comtrade");
	}, WaveformExportStatus::unprocessable,
		"malformed MNCWF source was accepted");
	require_status([&] {
		(void)manager.status("admin",
			"12345678-1234-5234-9234-1234567890ab");
	}, WaveformExportStatus::not_found,
		"unknown owner-scoped job did not return not-found");
}

void initialization_failure_is_nonfatal()
{
	TemporaryTree tree;
	auto options = options_for(tree);
	options.source_root = tree.root() / "absent-source";
	WaveformExportTaskManager manager(std::move(options));
	const auto capabilities = manager.capabilities();
	require(!capabilities.healthy && !capabilities.unavailable_reason.empty(),
		"initialization failure did not suppress converted capabilities");
	require_status([&] {
		(void)manager.submit("admin", "1", "capture.mncwf", "capture", {},
			"comtrade");
	}, WaveformExportStatus::unavailable,
		"failed task manager accepted a conversion");
}

} // namespace

int main()
{
	try {
		healthy_lifecycle_and_validation();
		initialization_failure_is_nonfatal();
		std::cout << "PASS: waveform_export_task_manager_test\n";
		return 0;
	} catch (const std::exception &error) {
		std::cerr << "waveform_export_task_manager_test: " << error.what()
			<< '\n';
		return 1;
	}
}
