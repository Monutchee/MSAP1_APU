#include "waveform_conversion_service.hpp"
#include "ipc_access_policy.hpp"

#include "mnc/waveform/waveform_converter.hpp"
#include "msap1/waveform/mncwf_v4.hpp"
#include "msap1/waveform/mncwf_waveform_source.hpp"

#include <boost/uuid/random_generator.hpp>
#include <boost/uuid/uuid_io.hpp>

#include <glaze/glaze.hpp>
#include <openssl/evp.h>
#include <zlib.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <charconv>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <fcntl.h>
#include <iomanip>
#include <limits>
#include <optional>
#include <ranges>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <unistd.h>
#include <utility>

namespace msap1::waveform::daemon {
namespace {

using namespace std::chrono_literals;
using mnc::waveform::ConversionError;
using mnc::waveform::ConversionErrorCode;
using mnc::waveform::ConversionOptions;
using mnc::waveform::ExportFormat;
using mnc::waveform::ExportScope;

constexpr std::uint64_t maximum_output_bytes = 1024ull * 1024ull * 1024ull;
constexpr std::uint64_t minimum_free_bytes = 512ull * 1024ull * 1024ull;
constexpr std::size_t maximum_queued_jobs = 8;
constexpr auto artifact_ttl = 30min;
constexpr auto stream_lease = 30s;
constexpr std::size_t io_buffer_bytes = 64u * 1024u;

class ServiceFailure final : public std::runtime_error {
public:
	ServiceFailure(ipc::Status status, std::string message)
		: std::runtime_error(std::move(message)), status(status) {}
	ipc::Status status;
};

class StorageFull final : public std::runtime_error {
public:
	using std::runtime_error::runtime_error;
};

class Descriptor final {
public:
	explicit Descriptor(int value = -1) : value_(value) {}
	~Descriptor()
	{
		if (value_ >= 0)
			::close(value_);
	}
	Descriptor(const Descriptor &) = delete;
	Descriptor &operator=(const Descriptor &) = delete;
	Descriptor(Descriptor &&other) noexcept
		: value_(std::exchange(other.value_, -1)) {}
	Descriptor &operator=(Descriptor &&other) noexcept
	{
		if (this != &other) {
			if (value_ >= 0)
				::close(value_);
			value_ = std::exchange(other.value_, -1);
		}
		return *this;
	}
	[[nodiscard]] int get() const noexcept { return value_; }

private:
	int value_;
};

std::string encode_json(const auto &value)
{
	const auto encoded = glz::write_json(value);
	if (!encoded)
		throw std::runtime_error("cannot encode waveform conversion IPC JSON");
	return *encoded;
}

std::string iso_time(std::chrono::system_clock::time_point value)
{
	const auto nanoseconds = std::chrono::duration_cast<std::chrono::nanoseconds>(
		value.time_since_epoch());
	const auto seconds = std::chrono::duration_cast<std::chrono::seconds>(
		nanoseconds);
	auto fraction = nanoseconds - seconds;
	auto whole_seconds = seconds;
	if (fraction.count() < 0) {
		whole_seconds -= 1s;
		fraction += 1s;
	}
	const std::time_t time = static_cast<std::time_t>(whole_seconds.count());
	std::tm utc{};
	if (::gmtime_r(&time, &utc) == nullptr)
		throw std::runtime_error("cannot format waveform job timestamp");
	char prefix[32]{};
	if (std::strftime(prefix, sizeof(prefix), "%Y-%m-%dT%H:%M:%S", &utc) == 0)
		throw std::runtime_error("cannot format waveform job timestamp");
	std::ostringstream output;
	output << prefix << '.' << std::setw(9) << std::setfill('0')
	       << fraction.count() << 'Z';
	return output.str();
}

void validate_owner(std::string_view owner)
{
	if (owner.empty() || owner.size() > 128u ||
	    std::ranges::any_of(owner, [](unsigned char character) {
			return character < 0x20u || character == 0x7fu;
	    }))
		throw ServiceFailure(ipc::Status::invalid_request,
			"authenticated export owner is invalid");
}

void validate_basename(std::string_view name)
{
	if (name.empty() || name.size() > 255u || name == "." || name == ".." ||
	    name.find('/') != std::string_view::npos ||
	    name.find('\\') != std::string_view::npos ||
	    std::ranges::any_of(name, [](unsigned char character) {
		return !((character >= 'a' && character <= 'z') ||
			(character >= 'A' && character <= 'Z') ||
			(character >= '0' && character <= '9') ||
			character == '.' || character == '-' || character == '_');
	    }) ||
	    !name.ends_with(".mncwf"))
		throw ServiceFailure(ipc::Status::invalid_request,
			"source_basename must name one MNCWF file");
}

void validate_job_id(std::string_view job_id)
{
	const auto parsed = mncwf_uuid_from_string(job_id);
	if (!parsed || mncwf_uuid_is_zero(*parsed))
		throw ServiceFailure(ipc::Status::invalid_request,
			"job_id must be a nonzero canonical UUID");
}

void validate_session_id(std::string_view session_id)
{
	std::uint64_t parsed = 0;
	const auto result = std::from_chars(session_id.data(),
		session_id.data() + session_id.size(), parsed);
	if (session_id.empty() || result.ec != std::errc{} ||
	    result.ptr != session_id.data() + session_id.size() || parsed == 0u)
		throw ServiceFailure(ipc::Status::invalid_request,
			"session_id must be a positive integer");
}

std::string profile(ExportFormat format)
{
	switch (format) {
	case ExportFormat::comtrade:
		return "IEC 60255-24:2013 CFF/BINARY32";
	case ExportFormat::comtrade_zip:
		return "IEC 60255-24:2013 CFG/DAT ZIP (BINARY32)";
	case ExportFormat::pqdif:
		return "IEEE 1159.3-2025 PQDIF (normative definitions 1.0.0)";
	}
	throw ServiceFailure(ipc::Status::invalid_request,
		"unsupported waveform export format");
}

std::string extension(ExportFormat format)
{
	switch (format) {
	case ExportFormat::comtrade: return ".cff";
	case ExportFormat::comtrade_zip: return ".zip";
	case ExportFormat::pqdif: return ".pqd";
	}
	return {};
}

std::string output_stem(std::string_view source_basename,
	ExportScope scope, std::string_view event_id)
{
	std::string result(source_basename.substr(0,
		source_basename.size() - std::string_view(".mncwf").size()));
	if (scope == ExportScope::event) {
		result += "-event-";
		result.append(event_id.substr(0, std::min<std::size_t>(8, event_id.size())));
	}
	return result;
}

std::uint32_t read_u32(std::span<const std::byte> bytes, std::size_t offset)
{
	if (offset > bytes.size() || bytes.size() - offset < 4u)
		throw std::runtime_error("artifact validation read is truncated");
	std::uint32_t value = 0;
	for (unsigned index = 0; index != 4; ++index)
		value |= std::to_integer<std::uint32_t>(bytes[offset + index]) <<
			(index * 8u);
	return value;
}

void read_exact(int fd, std::uint64_t offset, std::span<std::byte> destination)
{
	std::size_t done = 0;
	while (done != destination.size()) {
		const auto count = ::pread(fd, destination.data() + done,
			destination.size() - done, static_cast<off_t>(offset + done));
		if (count < 0 && errno == EINTR)
			continue;
		if (count <= 0)
			throw std::runtime_error("artifact validation read failed");
		done += static_cast<std::size_t>(count);
	}
}

std::uint32_t checksum_region(int fd, std::uint64_t offset,
	std::uint64_t bytes)
{
	uLong checksum = ::adler32(0L, Z_NULL, 0);
	std::array<std::byte, io_buffer_bytes> buffer{};
	while (bytes != 0u) {
		const auto count = static_cast<std::size_t>(std::min<std::uint64_t>(
			bytes, buffer.size()));
		read_exact(fd, offset, std::span{buffer}.first(count));
		checksum = ::adler32(checksum,
			reinterpret_cast<const Bytef *>(buffer.data()), count);
		offset += count;
		bytes -= count;
	}
	return static_cast<std::uint32_t>(checksum);
}

void validate_pqdif(int fd, std::uint64_t bytes)
{
	constexpr std::array<std::byte, 16> signature{
		std::byte{0x40}, std::byte{0x14}, std::byte{0x11}, std::byte{0x4a},
		std::byte{0x9f}, std::byte{0xe4}, std::byte{0xcf}, std::byte{0x11},
		std::byte{0x99}, std::byte{0x00}, std::byte{0x50}, std::byte{0x51},
		std::byte{0x44}, std::byte{0x49}, std::byte{0x46}, std::byte{0x00}};
	std::uint64_t offset = 0;
	std::size_t records = 0;
	while (offset < bytes) {
		std::array<std::byte, 64> header{};
		read_exact(fd, offset, header);
		if (!std::equal(signature.begin(), signature.end(), header.begin()) ||
		    read_u32(header, 32u) != 64u)
			throw std::runtime_error("PQDIF record header validation failed");
		const auto body = read_u32(header, 36u);
		const auto next = read_u32(header, 40u);
		const auto checksum = read_u32(header, 44u);
		if (body == 0u || body > bytes - offset - 64u ||
		    checksum_region(fd, offset + 64u, body) != checksum)
			throw std::runtime_error("PQDIF record body validation failed");
		const auto expected = offset + 64u + body;
		++records;
		if (next == 0u) {
			if (expected != bytes)
				throw std::runtime_error("PQDIF final record link is invalid");
			offset = expected;
		} else {
			if (next != expected)
				throw std::runtime_error("PQDIF next-record link is invalid");
			offset = next;
		}
	}
	if (records != 4u)
		throw std::runtime_error("PQDIF export does not contain four records");
}

void validate_artifact(int fd, ExportFormat format, std::uint64_t bytes)
{
	if (bytes < 22u)
		throw std::runtime_error("waveform artifact is truncated");
	std::array<std::byte, 32> first{};
	read_exact(fd, 0, first);
	if (format == ExportFormat::comtrade) {
		constexpr std::string_view marker = "--- file type: CFG ---\r\n";
		if (!std::equal(marker.begin(), marker.end(),
			reinterpret_cast<const char *>(first.data())))
			throw std::runtime_error("COMTRADE CFF validation failed");
		return;
	}
	if (format == ExportFormat::comtrade_zip) {
		if (first[0] != std::byte{'P'} || first[1] != std::byte{'K'} ||
		    first[2] != std::byte{3} || first[3] != std::byte{4})
			throw std::runtime_error("COMTRADE ZIP local header is invalid");
		std::array<std::byte, 22> ending{};
		read_exact(fd, bytes - ending.size(), ending);
		if (read_u32(ending, 0) != 0x06054b50u ||
		    read_u32(ending, 8) != 0x00020002u)
			throw std::runtime_error("COMTRADE ZIP central directory is invalid");
		return;
	}
	validate_pqdif(fd, bytes);
}

std::string sha256_file(int fd, std::uint64_t bytes)
{
	EVP_MD_CTX *context = EVP_MD_CTX_new();
	if (!context)
		throw std::runtime_error("cannot allocate artifact SHA-256 context");
	std::array<unsigned char, EVP_MAX_MD_SIZE> digest{};
	unsigned digest_size = 0;
	std::array<std::byte, io_buffer_bytes> buffer{};
	try {
		if (EVP_DigestInit_ex(context, EVP_sha256(), nullptr) != 1)
			throw std::runtime_error("cannot initialize artifact SHA-256");
		std::uint64_t offset = 0;
		while (offset != bytes) {
			const auto count = static_cast<std::size_t>(std::min<std::uint64_t>(
				buffer.size(), bytes - offset));
			read_exact(fd, offset, std::span{buffer}.first(count));
			if (EVP_DigestUpdate(context, buffer.data(), count) != 1)
				throw std::runtime_error("cannot update artifact SHA-256");
			offset += count;
		}
		if (EVP_DigestFinal_ex(context, digest.data(), &digest_size) != 1 ||
		    digest_size != 32u)
			throw std::runtime_error("cannot finalize artifact SHA-256");
		EVP_MD_CTX_free(context);
	} catch (...) {
		EVP_MD_CTX_free(context);
		throw;
	}
	std::ostringstream output;
	output << std::hex << std::setfill('0');
	for (unsigned index = 0; index != digest_size; ++index)
		output << std::setw(2) << static_cast<unsigned>(digest[index]);
	return output.str();
}

std::uint64_t available_bytes(const std::filesystem::path &directory)
{
	struct statvfs status {};
	if (::statvfs(directory.c_str(), &status) != 0)
		throw std::system_error(errno, std::generic_category(),
			"inspect waveform export filesystem");
	const auto blocks = static_cast<std::uint64_t>(status.f_bavail);
	const auto size = static_cast<std::uint64_t>(status.f_frsize);
	if (size != 0u && blocks > std::numeric_limits<std::uint64_t>::max() / size)
		return std::numeric_limits<std::uint64_t>::max();
	return blocks * size;
}

} // namespace

struct WaveformConversionService::JobRecord {
	ipc::Job dto;
	std::string key;
	std::string artifact_basename;
	std::shared_ptr<MncwfWaveformSource> source;
	ExportFormat format = ExportFormat::comtrade;
	ExportScope scope = ExportScope::capture;
	std::optional<MncwfUuid> event_uuid;
	std::stop_source stop;
	bool streaming = false;
	std::chrono::steady_clock::time_point stream_activity{};
	std::chrono::system_clock::time_point created{};
	std::chrono::system_clock::time_point completed{};
};

class WaveformConversionService::FileSink final
	: public mnc::waveform::OutputSink {
public:
	FileSink(WaveformConversionService &service,
		std::shared_ptr<JobRecord> job)
		: service_(service), job_(std::move(job)),
		  part_(job_->dto.job_id + ".part")
	{
		directory_ = Descriptor(::open(service_.export_root_.c_str(),
			O_RDONLY | O_CLOEXEC | O_DIRECTORY));
		if (directory_.get() < 0)
			throw std::system_error(errno, std::generic_category(),
				"open waveform export directory");
		file_ = Descriptor(::openat(directory_.get(), part_.c_str(),
			O_RDWR | O_CLOEXEC | O_CREAT | O_EXCL | O_NOFOLLOW, 0600));
		if (file_.get() < 0)
			throw std::system_error(errno, std::generic_category(),
				"create waveform export partial file");
	}

	~FileSink() override
	{
		if (!published_ && directory_.get() >= 0)
			(void)::unlinkat(directory_.get(), part_.c_str(), 0);
	}

	void write(std::span<const std::byte> bytes) override
	{
		{
			std::scoped_lock lock(service_.mutex_);
			service_.prepare_write_locked(job_, bytes_, bytes.size());
		}
		std::size_t offset = 0;
		while (offset != bytes.size()) {
			const auto written = ::write(file_.get(), bytes.data() + offset,
				bytes.size() - offset);
			if (written < 0 && errno == EINTR)
				continue;
			if (written <= 0)
				throw std::system_error(errno, std::generic_category(),
					"write waveform export");
			offset += static_cast<std::size_t>(written);
		}
		bytes_ += bytes.size();
	}

	[[nodiscard]] std::uint64_t bytes_written() const noexcept override
	{
		return bytes_;
	}
	[[nodiscard]] std::uint64_t byte_limit() const noexcept override
	{
		return maximum_output_bytes;
	}

	[[nodiscard]] int descriptor() const noexcept { return file_.get(); }

	void publish(std::string_view final_basename)
	{
		if (::fsync(file_.get()) != 0)
			throw std::system_error(errno, std::generic_category(),
				"synchronize waveform export");
		std::string final(final_basename);
		if (::renameat(directory_.get(), part_.c_str(), directory_.get(),
			final.c_str()) != 0)
			throw std::system_error(errno, std::generic_category(),
				"publish waveform export");
		if (::fsync(directory_.get()) != 0)
			throw std::system_error(errno, std::generic_category(),
				"synchronize waveform export directory");
		published_ = true;
	}

private:
	WaveformConversionService &service_;
	std::shared_ptr<JobRecord> job_;
	std::string part_;
	Descriptor directory_;
	Descriptor file_;
	std::uint64_t bytes_ = 0;
	bool published_ = false;
};

WaveformConversionService::WaveformConversionService()
	: Service("MSAP1 Waveform Converter", "waveform-converter"),
	  server_(context_.get_executor(), std::string(ipc::socket_path))
{
}

void WaveformConversionService::purge_artifacts()
{
	std::filesystem::create_directories(export_root_);
	std::filesystem::permissions(export_root_,
		std::filesystem::perms::owner_all,
		std::filesystem::perm_options::replace);
	Descriptor directory(::open(export_root_.c_str(),
		O_RDONLY | O_CLOEXEC | O_DIRECTORY));
	if (directory.get() < 0)
		throw std::system_error(errno, std::generic_category(),
			"open waveform export directory");
	for (const auto &entry : std::filesystem::directory_iterator(export_root_))
		std::filesystem::remove_all(entry.path());
	if (::fsync(directory.get()) != 0)
		throw std::system_error(errno, std::generic_category(),
			"synchronize purged waveform export directory");
}

void WaveformConversionService::on_start()
{
	purge_artifacts();
	server_.start([this](auto connection, auto frame) {
		handle(std::move(connection), std::move(frame));
	}, [this](const std::string &error) {
		(void)logger().write(mnc::logging::Priority::warning,
			"Waveform converter IPC error: " + error, "ipc_error");
	});
	if (::chmod(ipc::socket_path.data(), 0660) != 0)
		throw std::system_error(errno, std::generic_category(),
			"set waveform converter socket mode");
	io_worker_ = std::thread([this] {
		try {
			context_.run();
		} catch (const std::exception &error) {
			{
				std::scoped_lock lock(failure_mutex_);
				failure_message_ = error.what();
			}
			worker_failed_ = true;
			request_stop();
		}
	});
	conversion_worker_ = std::thread([this] { worker_loop(); });
}

void WaveformConversionService::on_reload()
{
	std::scoped_lock lock(mutex_);
	cleanup_expired_locked();
}

void WaveformConversionService::on_stop() noexcept
{
	stopping_ = true;
	{
		std::scoped_lock lock(mutex_);
		for (const auto &[id, job] : jobs_)
			job->stop.request_stop();
	}
	condition_.notify_all();
	if (conversion_worker_.joinable())
		conversion_worker_.join();
	server_.stop();
	context_.stop();
	if (io_worker_.joinable())
		io_worker_.join();
}

mnc::ServiceHealth WaveformConversionService::health() const
{
	if (worker_failed_) {
		std::scoped_lock lock(failure_mutex_);
		return {false, "waveform converter worker failed: " + failure_message_};
	}
	std::scoped_lock lock(mutex_);
	return {true, "ready; queued=" + std::to_string(queue_.size())};
}

void WaveformConversionService::update_queue_positions_locked()
{
	std::uint32_t position = 1;
	for (const auto &job : queue_)
		job->dto.queue_position = position++;
}

void WaveformConversionService::cleanup_expired_locked()
{
	const auto now = std::chrono::system_clock::now();
	const auto steady_now = std::chrono::steady_clock::now();
	for (auto iterator = jobs_.begin(); iterator != jobs_.end();) {
		const auto &job = iterator->second;
		if (job->streaming && job->stream_activity !=
		    std::chrono::steady_clock::time_point{} &&
		    steady_now - job->stream_activity >= stream_lease)
			job->streaming = false;
		const bool terminal = job->dto.state == "ready" ||
			job->dto.state == "failed" || job->dto.state == "cancelled";
		if (terminal && !job->streaming && job->completed !=
		    std::chrono::system_clock::time_point{} &&
		    now - job->completed >= artifact_ttl) {
			if (!job->artifact_basename.empty()) {
				std::error_code error;
				(void)std::filesystem::remove(
					export_root_ / job->artifact_basename, error);
				if (error) {
					++iterator;
					continue;
				}
			}
			iterator = jobs_.erase(iterator);
		} else {
			++iterator;
		}
	}
}

void WaveformConversionService::evict_locked(
	const std::shared_ptr<JobRecord> &job)
{
	if (!job || job->dto.state != "ready" || job->streaming)
		return;
	std::error_code error;
	(void)std::filesystem::remove(export_root_ / job->artifact_basename, error);
	if (error)
		throw StorageFull(
			"cannot evict the oldest ready waveform export artifact");
	job->dto.state = "failed";
	job->dto.error_code = "artifact_evicted";
	job->dto.error_message = "export artifact was evicted to satisfy storage policy";
	job->dto.bytes = 0;
	job->dto.sha256.clear();
	job->artifact_basename.clear();
}

void WaveformConversionService::prepare_write_locked(
	const std::shared_ptr<JobRecord> &job, std::uint64_t current_bytes,
	std::size_t additional_bytes)
{
	cleanup_expired_locked();
	if (additional_bytes > maximum_output_bytes - current_bytes)
		throw ConversionError(ConversionErrorCode::output_too_large,
			"waveform export exceeds the 1 GiB output limit");
	for (;;) {
		std::uint64_t ready_bytes = 0;
		for (const auto &[id, candidate] : jobs_)
			if (candidate != job && candidate->dto.state == "ready")
				ready_bytes += candidate->dto.bytes;
		const auto free = available_bytes(export_root_);
		const bool quota_ok = ready_bytes <= maximum_output_bytes -
			current_bytes - additional_bytes;
		const bool reserve_ok = free >= minimum_free_bytes + additional_bytes;
		if (quota_ok && reserve_ok)
			return;
		std::shared_ptr<JobRecord> oldest;
		for (const auto &[id, candidate] : jobs_) {
			if (candidate == job || candidate->dto.state != "ready" ||
			    candidate->streaming)
				continue;
			if (!oldest || candidate->completed < oldest->completed)
				oldest = candidate;
		}
		if (!oldest)
			throw StorageFull("waveform export storage policy cannot reserve output space");
		evict_locked(oldest);
	}
}

std::shared_ptr<WaveformConversionService::JobRecord>
WaveformConversionService::submit(const ipc::Request &request)
{
	validate_owner(request.owner);
	validate_basename(request.source_basename);
	validate_session_id(request.session_id);
	const auto parsed_format = mnc::waveform::export_format_from_name(
		request.format);
	if (!parsed_format)
		throw ServiceFailure(ipc::Status::invalid_request,
			"format must be comtrade, comtrade-zip, or pqdif");
	ExportScope scope;
	if (request.scope == "capture") scope = ExportScope::capture;
	else if (request.scope == "event") scope = ExportScope::event;
	else throw ServiceFailure(ipc::Status::invalid_request,
		"scope must be capture or event");
	std::optional<MncwfUuid> event_uuid;
	if (scope == ExportScope::event) {
		event_uuid = mncwf_uuid_from_string(request.event_id);
		if (!event_uuid)
			throw ServiceFailure(ipc::Status::invalid_request,
				"event scope requires a canonical event_id");
	} else if (!request.event_id.empty()) {
		throw ServiceFailure(ipc::Status::invalid_request,
			"event_id is only valid for event scope");
	}
	const auto key = request.owner + '\n' + request.source_basename + '\n' +
		request.scope + '\n' + request.event_id + '\n' + request.format;
	{
		std::scoped_lock lock(mutex_);
		cleanup_expired_locked();
		for (const auto &[id, existing] : jobs_)
			if (existing->key == key && (existing->dto.state == "queued" ||
			    existing->dto.state == "running" || existing->dto.state == "ready"))
				return existing;
		if (queue_.size() >= maximum_queued_jobs)
			throw ServiceFailure(ipc::Status::queue_full,
				"waveform conversion queue is full");
	}

	Descriptor directory(::open(source_root_.c_str(),
		O_RDONLY | O_CLOEXEC | O_DIRECTORY));
	if (directory.get() < 0)
		throw std::system_error(errno, std::generic_category(),
			"open waveform source directory");
	Descriptor source_fd(::openat(directory.get(), request.source_basename.c_str(),
		O_RDONLY | O_CLOEXEC | O_NOFOLLOW));
	if (source_fd.get() < 0) {
		if (errno == ENOENT)
			throw ServiceFailure(ipc::Status::not_found,
				"selected MNCWF source does not exist");
		throw std::system_error(errno, std::generic_category(),
			"open selected MNCWF source");
	}
	struct stat source_status {};
	if (::fstat(source_fd.get(), &source_status) != 0)
		throw std::system_error(errno, std::generic_category(),
			"inspect selected MNCWF source");
	if (!S_ISREG(source_status.st_mode) || source_status.st_size <= 0 ||
	    static_cast<std::uint64_t>(source_status.st_size) > mncwf_v4_max_file_bytes)
		throw ServiceFailure(ipc::Status::unprocessable,
			"selected MNCWF source is not a bounded regular file");

	std::shared_ptr<MncwfWaveformSource> source;
	try {
		source = std::make_shared<MncwfWaveformSource>(source_fd.get(),
			*parsed_format, scope, event_uuid);
	} catch (const ConversionError &) {
		throw;
	} catch (const std::exception &error) {
		throw ServiceFailure(ipc::Status::unprocessable,
			"selected MNCWF source is invalid: " + std::string(error.what()));
	}

	const auto now = std::chrono::system_clock::now();
	auto job = std::make_shared<JobRecord>();
	job->dto.job_id = boost::uuids::to_string(boost::uuids::random_generator()());
	job->dto.state = "queued";
	job->dto.owner = request.owner;
	job->dto.session_id = request.session_id;
	job->dto.source_basename = request.source_basename;
	job->dto.scope = request.scope;
	job->dto.event_id = request.event_id;
	job->dto.format = request.format;
	job->dto.profile = profile(*parsed_format);
	job->dto.total_frames = source->frame_count();
	job->dto.created_at = iso_time(now);
	job->created = now;
	job->key = key;
	job->source = std::move(source);
	job->format = *parsed_format;
	job->scope = scope;
	job->event_uuid = event_uuid;
	const auto stem = output_stem(request.source_basename, scope,
		request.event_id);
	job->dto.filename = stem + extension(*parsed_format);
	job->artifact_basename = job->dto.job_id + extension(*parsed_format);

	{
		std::scoped_lock lock(mutex_);
		cleanup_expired_locked();
		for (const auto &[id, existing] : jobs_)
			if (existing->key == key && (existing->dto.state == "queued" ||
			    existing->dto.state == "running" || existing->dto.state == "ready"))
				return existing;
		if (queue_.size() >= maximum_queued_jobs)
			throw ServiceFailure(ipc::Status::queue_full,
				"waveform conversion queue is full");
		prepare_write_locked(job, 0, 1);
		jobs_.emplace(job->dto.job_id, job);
		queue_.push_back(job);
		update_queue_positions_locked();
	}
	condition_.notify_one();
	return job;
}

std::shared_ptr<WaveformConversionService::JobRecord>
WaveformConversionService::find_owned(std::string_view job_id,
	std::string_view owner)
{
	validate_owner(owner);
	validate_job_id(job_id);
	std::scoped_lock lock(mutex_);
	cleanup_expired_locked();
	const auto found = jobs_.find(std::string(job_id));
	if (found == jobs_.end() || found->second->dto.owner != owner)
		throw ServiceFailure(ipc::Status::not_found,
			"waveform export job was not found");
	return found->second;
}

std::shared_ptr<WaveformConversionService::JobRecord>
WaveformConversionService::cancel(const ipc::Request &request)
{
	validate_owner(request.owner);
	validate_job_id(request.job_id);
	std::scoped_lock lock(mutex_);
	cleanup_expired_locked();
	const auto found = jobs_.find(request.job_id);
	if (found == jobs_.end() || found->second->dto.owner != request.owner)
		throw ServiceFailure(ipc::Status::not_found,
			"waveform export job was not found");
	const auto job = found->second;
	if (job->dto.state == "queued") {
		std::erase(queue_, job);
		update_queue_positions_locked();
	}
	if (job->dto.state == "running")
		job->stop.request_stop();
	if (job->dto.state == "ready" && job->streaming)
		throw ServiceFailure(ipc::Status::conflict,
			"waveform export is currently being downloaded");
	if (job->dto.state == "ready" && !job->artifact_basename.empty()) {
		std::error_code error;
		(void)std::filesystem::remove(export_root_ / job->artifact_basename, error);
		if (error)
			throw std::system_error(error,
				"discard waveform export artifact");
		job->artifact_basename.clear();
	}
	const auto now = std::chrono::system_clock::now();
	job->dto.state = "cancelled";
	job->dto.queue_position = 0;
	job->dto.bytes = 0;
	job->dto.sha256.clear();
	job->dto.error_code = "conversion_cancelled";
	job->dto.error_message = "waveform export was cancelled or discarded";
	job->dto.completed_at = iso_time(now);
	job->dto.expires_at = iso_time(now + artifact_ttl);
	job->completed = now;
	return job;
}

ipc::Response WaveformConversionService::read_chunk(const ipc::Request &request)
{
	validate_owner(request.owner);
	validate_job_id(request.job_id);
	if (request.limit == 0u || request.limit > ipc::maximum_chunk_bytes)
		throw ServiceFailure(ipc::Status::invalid_request,
			"download chunk limit must be 1..524288");
	std::shared_ptr<JobRecord> job;
	std::string artifact;
	std::string filename;
	std::string sha256;
	std::uint64_t total_size = 0;
	ExportFormat format = ExportFormat::comtrade;
	{
		std::scoped_lock lock(mutex_);
		cleanup_expired_locked();
		const auto found = jobs_.find(request.job_id);
		if (found == jobs_.end() || found->second->dto.owner != request.owner)
			throw ServiceFailure(ipc::Status::not_found,
				"waveform export job was not found");
		job = found->second;
		if (job->dto.state != "ready")
			throw ServiceFailure(ipc::Status::conflict,
				"waveform export is not ready for download");
		if (request.offset > job->dto.bytes)
			throw ServiceFailure(ipc::Status::invalid_request,
				"download offset exceeds artifact size");
		job->streaming = true;
		job->stream_activity = std::chrono::steady_clock::now();
		artifact = job->artifact_basename;
		filename = job->dto.filename;
		sha256 = job->dto.sha256;
		total_size = job->dto.bytes;
		format = job->format;
	}

	try {
		Descriptor directory(::open(export_root_.c_str(),
			O_RDONLY | O_CLOEXEC | O_DIRECTORY));
		Descriptor file(directory.get() < 0 ? -1 : ::openat(directory.get(),
			artifact.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW));
		if (file.get() < 0)
			throw std::runtime_error("waveform export artifact is unavailable");
		struct stat artifact_status {};
		if (::fstat(file.get(), &artifact_status) != 0 ||
		    !S_ISREG(artifact_status.st_mode) || artifact_status.st_size < 0 ||
		    static_cast<std::uint64_t>(artifact_status.st_size) != total_size)
			throw std::runtime_error(
				"waveform export artifact identity is invalid");
		const auto count = static_cast<std::size_t>(std::min<std::uint64_t>(
			request.limit, total_size - request.offset));
		std::string content(count, '\0');
		std::size_t read = 0;
		while (read != count) {
			const auto result = ::pread(file.get(), content.data() + read,
				count - read, static_cast<off_t>(request.offset + read));
			if (result < 0 && errno == EINTR)
				continue;
			if (result <= 0)
				throw std::runtime_error("waveform export artifact read failed");
			read += static_cast<std::size_t>(result);
		}
		const bool eof = request.offset + count == total_size;
		if (eof) {
			std::scoped_lock lock(mutex_);
			job->streaming = false;
		}
		ipc::Response response;
		response.content = std::move(content);
		response.filename = std::move(filename);
		response.media_type = format == ExportFormat::comtrade
			? "application/vnd.iec.comtrade"
			: format == ExportFormat::comtrade_zip
				? "application/zip" : "application/vnd.pqdif";
		response.sha256 = std::move(sha256);
		response.total_size = total_size;
		response.end_of_file = eof;
		return response;
	} catch (...) {
		std::scoped_lock lock(mutex_);
		job->streaming = false;
		throw;
	}
}

void WaveformConversionService::convert(const std::shared_ptr<JobRecord> &job)
{
	try {
		FileSink sink(*this, job);
		ConversionOptions options;
		options.format = job->format;
		options.scope = job->scope;
		options.maximum_output_bytes = maximum_output_bytes;
		options.output_stem = output_stem(job->dto.source_basename,
			job->scope, job->dto.event_id);
		if (job->event_uuid)
			options.selected_event_uuid = *job->event_uuid;
		const auto converter = mnc::waveform::make_converter(job->format);
		const auto summary = converter->convert(*job->source, sink, options,
			job->stop.get_token(), [this, job](const auto &value) {
				std::scoped_lock lock(mutex_);
				if (job->dto.state == "running") {
					job->dto.processed_frames = value.processed_frames;
					job->dto.total_frames = value.total_frames;
				}
			});
		if (job->stop.stop_requested())
			throw ConversionError(ConversionErrorCode::conversion_cancelled,
				"waveform conversion was cancelled");
		validate_artifact(sink.descriptor(), job->format, summary.bytes);
		const auto sha256 = sha256_file(sink.descriptor(), summary.bytes);
		sink.publish(job->artifact_basename);
		const auto now = std::chrono::system_clock::now();
		std::scoped_lock lock(mutex_);
		if (job->dto.state == "cancelled") {
			std::error_code error;
			(void)std::filesystem::remove(
				export_root_ / job->artifact_basename, error);
			if (error)
				throw std::system_error(error,
					"remove cancelled waveform export artifact");
			job->artifact_basename.clear();
			return;
		}
		job->dto.state = "ready";
		job->dto.profile = summary.profile;
		job->dto.queue_position = 0;
		job->dto.processed_frames = summary.frames;
		job->dto.total_frames = summary.frames;
		job->dto.bytes = summary.bytes;
		job->dto.sha256 = sha256;
		job->dto.completed_at = iso_time(now);
		job->dto.expires_at = iso_time(now + artifact_ttl);
		job->completed = now;
	} catch (const ConversionError &error) {
		const auto now = std::chrono::system_clock::now();
		std::scoped_lock lock(mutex_);
		job->dto.state = error.code() == ConversionErrorCode::conversion_cancelled
			? "cancelled" : "failed";
		job->dto.queue_position = 0;
		job->dto.error_code = std::string(
			mnc::waveform::conversion_error_code_name(error.code()));
		job->dto.error_message = error.what();
		job->dto.missing_fields = error.missing_fields();
		job->dto.completed_at = iso_time(now);
		job->dto.expires_at = iso_time(now + artifact_ttl);
		job->completed = now;
	} catch (const StorageFull &error) {
		const auto now = std::chrono::system_clock::now();
		std::scoped_lock lock(mutex_);
		job->dto.state = "failed";
		job->dto.queue_position = 0;
		job->dto.error_code = "storage_full";
		job->dto.error_message = error.what();
		job->dto.completed_at = iso_time(now);
		job->dto.expires_at = iso_time(now + artifact_ttl);
		job->completed = now;
	} catch (const std::exception &error) {
		const auto now = std::chrono::system_clock::now();
		std::scoped_lock lock(mutex_);
		job->dto.state = "failed";
		job->dto.queue_position = 0;
		job->dto.error_code = "internal_error";
		job->dto.error_message = error.what();
		job->dto.completed_at = iso_time(now);
		job->dto.expires_at = iso_time(now + artifact_ttl);
		job->completed = now;
	}
}

void WaveformConversionService::worker_loop()
{
	try {
		while (!stopping_) {
			std::shared_ptr<JobRecord> job;
			{
				std::unique_lock lock(mutex_);
				condition_.wait(lock, [this] {
					return stopping_.load() || !queue_.empty();
				});
				if (stopping_)
					break;
				job = queue_.front();
				queue_.pop_front();
				update_queue_positions_locked();
				if (job->dto.state == "cancelled")
					continue;
				const auto now = std::chrono::system_clock::now();
				job->dto.state = "running";
				job->dto.queue_position = 0;
				job->dto.started_at = iso_time(now);
			}
			convert(job);
		}
	} catch (const std::exception &error) {
		{
			std::scoped_lock lock(failure_mutex_);
			failure_message_ = error.what();
		}
		worker_failed_ = true;
		request_stop();
	}
}

void WaveformConversionService::handle(
	mnc::ipc::UnixStreamServer::Connection connection, mnc::ipc::Frame frame)
{
	ipc::Response response;
	ipc::Command command = ipc::Command::capabilities;
	try {
		const auto request = ipc::decode_request(frame);
		command = request.command;
		if (!peer_authorized(connection->peer_credentials()))
			throw ServiceFailure(ipc::Status::permission_denied,
				"waveform converter command is not authorized for this peer");
		switch (command) {
		case ipc::Command::capabilities:
			response.json = encode_json(ipc::Capabilities{});
			break;
		case ipc::Command::submit: {
			const auto job = submit(request);
			std::scoped_lock lock(mutex_);
			response.json = encode_json(job->dto);
			break;
		}
		case ipc::Command::status: {
			const auto job = find_owned(request.job_id, request.owner);
			std::scoped_lock lock(mutex_);
			response.json = encode_json(job->dto);
			break;
		}
		case ipc::Command::read_chunk:
			response = read_chunk(request);
			break;
		case ipc::Command::cancel: {
			const auto job = cancel(request);
			std::scoped_lock lock(mutex_);
			response.json = encode_json(job->dto);
			break;
		}
		}
	} catch (const ServiceFailure &error) {
		response.status = error.status;
		response.message = error.what();
	} catch (const ConversionError &error) {
		response.status = error.code() == ConversionErrorCode::source_event_not_found
			? ipc::Status::not_found : ipc::Status::unprocessable;
		response.message = error.what();
		response.json = encode_json(ipc::ErrorDetail{
			std::string(mnc::waveform::conversion_error_code_name(error.code())),
			error.missing_fields()});
	} catch (const StorageFull &error) {
		response.status = ipc::Status::storage_full;
		response.message = error.what();
	} catch (const std::invalid_argument &error) {
		response.status = ipc::Status::invalid_request;
		response.message = error.what();
	} catch (const std::exception &error) {
		response.status = ipc::Status::internal_error;
		response.message = error.what();
	}
	connection->post_send(ipc::encode_response(response,
		frame.correlation_id, command));
}

} // namespace msap1::waveform::daemon
