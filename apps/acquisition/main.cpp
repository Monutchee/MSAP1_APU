#include "msap1/acquisition_ipc.hpp"
#include "msap1/protocol.hpp"
#include "msap1/rpmsg_endpoint.hpp"
#include "msap1/shared_ring.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

namespace {

using namespace std::chrono_literals;
using Clock = std::chrono::steady_clock;

volatile std::sig_atomic_t stop_requested = 0;

void handle_signal(int)
{
	stop_requested = 1;
}

[[noreturn]] void throw_errno(const std::string &operation)
{
	throw std::runtime_error(operation + ": " + std::strerror(errno));
}

struct Options {
	std::string service = "mncos-r5c0-ctrl";
	std::string rpmsg_device;
	std::string iio_device;
	std::string iio_sysfs_root = "/sys/bus/iio/devices";
	std::string socket_path = msap1::acquisition_socket_path;
	std::string shm_name = msap1::acquisition_shm_name;
};

void usage(const char *program)
{
	std::cerr
		<< "Usage: " << program << " [options]\n"
		<< "  --service NAME       RPMsg service (default: mncos-r5c0-ctrl)\n"
		<< "  --rpmsg-device PATH  Use an existing /dev/rpmsgN endpoint\n"
		<< "  --iio-device PATH    Use an existing /dev/iio:deviceN\n"
		<< "  --iio-sysfs PATH     IIO sysfs root\n"
		<< "  --socket PATH        Control socket path\n"
		<< "  --shm NAME           POSIX shared-memory name\n";
}

Options parse_options(int argc, char **argv)
{
	Options options;
	for (int index = 1; index < argc; ++index) {
		const std::string option = argv[index];
		if (option == "--help" || option == "-h") {
			usage(argv[0]);
			std::exit(0);
		}
		if (index + 1 >= argc)
			throw std::invalid_argument(option + " requires a value");
		const std::string value = argv[++index];
		if (option == "--service")
			options.service = value;
		else if (option == "--rpmsg-device")
			options.rpmsg_device = value;
		else if (option == "--iio-device")
			options.iio_device = value;
		else if (option == "--iio-sysfs")
			options.iio_sysfs_root = value;
		else if (option == "--socket")
			options.socket_path = value;
		else if (option == "--shm")
			options.shm_name = value;
		else
			throw std::invalid_argument("unknown option '" + option + "'");
	}
	return options;
}

std::string read_text(const std::filesystem::path &path)
{
	std::ifstream stream(path);
	if (!stream)
		throw std::runtime_error("cannot read " + path.string());
	std::string value;
	std::getline(stream, value);
	return value;
}

void write_text(const std::filesystem::path &path, const std::string &value)
{
	std::ofstream stream(path);
	if (!stream)
		throw std::runtime_error("cannot write " + path.string());
	stream << value;
	if (!stream)
		throw std::runtime_error("failed to write " + path.string());
}

class IioDevice {
public:
	explicit IioDevice(const Options &options)
	{
		if (!options.iio_device.empty()) {
			device_path_ = options.iio_device;
			const auto name = std::filesystem::path(device_path_).filename();
			sysfs_path_ = std::filesystem::path(options.iio_sysfs_root) / name;
			return;
		}
		for (const auto &entry :
		     std::filesystem::directory_iterator(options.iio_sysfs_root)) {
			if (!entry.is_directory() ||
			    entry.path().filename().string().rfind("iio:device", 0) != 0)
				continue;
			if (read_text(entry.path() / "name") != "msap1-ad7771")
				continue;
			sysfs_path_ = entry.path();
			device_path_ = "/dev/" + entry.path().filename().string();
			break;
		}
		if (device_path_.empty())
			throw std::runtime_error("msap1-ad7771 IIO device was not found");
	}

	~IioDevice() { stop(); }

	void start()
	{
		if (fd_ >= 0)
			return;
		for (std::size_t channel = 0; channel < msap1::adc_channel_count;
		     ++channel)
			write_text(sysfs_path_ / "scan_elements" /
				("in_voltage" + std::to_string(channel) + "_en"), "1");
		write_text(sysfs_path_ / "buffer" / "length", "512");
		fd_ = ::open(device_path_.c_str(), O_RDONLY | O_NONBLOCK | O_CLOEXEC);
		if (fd_ < 0)
			throw_errno("open " + device_path_);
		try {
			write_text(sysfs_path_ / "buffer" / "enable", "1");
		} catch (...) {
			::close(fd_);
			fd_ = -1;
			throw;
		}
	}

	void stop() noexcept
	{
		if (fd_ < 0)
			return;
		try {
			write_text(sysfs_path_ / "buffer" / "enable", "0");
		} catch (const std::exception &error) {
			std::cerr << "failed to disable IIO buffer: " << error.what() << '\n';
		}
		::close(fd_);
		fd_ = -1;
	}

	int fd() const { return fd_; }
	const std::string &device_path() const { return device_path_; }

private:
	std::filesystem::path sysfs_path_;
	std::string device_path_;
	int fd_ = -1;
};

class AcquisitionDaemon {
public:
	explicit AcquisitionDaemon(const Options &options)
		: options_(options), ring_(options.shm_name), iio_(options),
		  endpoint_(options.service, options.rpmsg_device)
	{
		create_socket();
	}

	~AcquisitionDaemon()
	{
		stop();
		if (listen_fd_ >= 0)
			::close(listen_fd_);
		if (!options_.socket_path.empty())
			::unlink(options_.socket_path.c_str());
	}

	void run()
	{
		start();
		std::cerr << "AD7771 acquisition started: " << iio_.device_path()
			  << " -> " << options_.shm_name << '\n';
		auto next_health = Clock::now() + 1s;

		while (!stop_requested) {
			pollfd descriptors[2] = {
				{iio_.fd(), POLLIN, 0},
				{listen_fd_, POLLIN, 0},
			};
			const int result = ::poll(descriptors, 2, 250);
			if (result < 0) {
				if (errno == EINTR)
					continue;
				throw_errno("poll acquisition devices");
			}
			if ((descriptors[0].revents & POLLIN) != 0)
				read_iio();
			if ((descriptors[0].revents & (POLLERR | POLLHUP | POLLNVAL)) != 0) {
				ring_.note_read_error();
				++iio_read_errors_;
			}
			if ((descriptors[1].revents & POLLIN) != 0)
				handle_client();

			if (running_ && Clock::now() >= next_health) {
				try {
					cached_health_ = query_rpu_health();
					ring_.set_capture_flags(cached_health_.capture_flags);
				} catch (const std::exception &error) {
					std::cerr << "RPU health query failed: " << error.what() << '\n';
				}
				next_health = Clock::now() + 1s;
			}
		}
	}

private:
	void create_socket()
	{
		const auto parent = std::filesystem::path(options_.socket_path).parent_path();
		if (!parent.empty())
			std::filesystem::create_directories(parent);
		listen_fd_ = ::socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0);
		if (listen_fd_ < 0)
			throw_errno("create acquisition control socket");
		::unlink(options_.socket_path.c_str());
		sockaddr_un address{};
		address.sun_family = AF_UNIX;
		if (options_.socket_path.size() >= sizeof(address.sun_path))
			throw std::invalid_argument("acquisition socket path is too long");
		std::memcpy(address.sun_path, options_.socket_path.c_str(),
			    options_.socket_path.size() + 1);
		if (::bind(listen_fd_, reinterpret_cast<sockaddr *>(&address),
			   sizeof(address)) < 0)
			throw_errno("bind " + options_.socket_path);
		(void)::chmod(options_.socket_path.c_str(), 0660);
		if (::listen(listen_fd_, 8) < 0)
			throw_errno("listen on acquisition socket");
	}

	msap1::Message transact(std::uint8_t type,
				std::chrono::milliseconds timeout = 1000ms)
	{
		const auto sequence = ++rpmsg_sequence_;
		endpoint_.send(msap1::encode_request(type, sequence));
		const auto deadline = Clock::now() + timeout;
		while (Clock::now() < deadline) {
			const auto remaining =
				std::chrono::duration_cast<std::chrono::milliseconds>(
					deadline - Clock::now());
			const auto frame = endpoint_.receive(remaining);
			if (frame.empty())
				continue;
			auto message = msap1::decode_message(frame.data(), frame.size());
			if (message.header.sequence != sequence)
				continue;
			if (message.header.type == MSAP1_RPU_MSG_ERROR ||
			    message.header.status != MSAP1_RPU_STATUS_OK)
				throw std::runtime_error("RPU rejected acquisition request (status " +
						 std::to_string(message.header.status) + ")");
			return message;
		}
		throw std::runtime_error("timed out waiting for RPU response");
	}

	msap1_adc_health_payload query_rpu_health()
	{
		return msap1::decode_adc_health(transact(MSAP1_RPU_MSG_ADC_HEALTH_GET));
	}

	void start()
	{
		if (running_)
			return;
		iio_.start();
		try {
			const auto response = transact(MSAP1_RPU_MSG_ADC_CAPTURE_START);
			if (response.header.type != MSAP1_RPU_MSG_ACK)
				throw std::runtime_error("unexpected RPU capture-start response");
		} catch (...) {
			iio_.stop();
			throw;
		}
		running_ = true;
		ring_.set_running(true);
	}

	void stop() noexcept
	{
		if (!running_)
			return;
		try {
			(void)transact(MSAP1_RPU_MSG_ADC_CAPTURE_STOP, 500ms);
		} catch (const std::exception &error) {
			std::cerr << "RPU capture stop failed: " << error.what() << '\n';
		}
		iio_.stop();
		running_ = false;
		ring_.set_running(false);
	}

	void read_iio()
	{
		std::array<unsigned char, 32768> bytes{};
		const auto size = ::read(iio_.fd(), bytes.data(), bytes.size());
		if (size < 0) {
			if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)
				return;
			ring_.note_read_error();
			++iio_read_errors_;
			return;
		}
		if (size == 0)
			return;

		iio_bytes_ += static_cast<std::uint64_t>(size);
		iio_blocks_ = iio_bytes_ / 8192u;
		pending_.insert(pending_.end(), bytes.begin(), bytes.begin() + size);
		const auto frame_count = pending_.size() / sizeof(msap1::AdcSampleFrame);
		if (frame_count == 0)
			return;
		std::vector<msap1::AdcSampleFrame> frames(frame_count);
		std::memcpy(frames.data(), pending_.data(),
			    frame_count * sizeof(msap1::AdcSampleFrame));
		ring_.publish(frames.data(), frames.size());
		pending_.erase(pending_.begin(),
			       pending_.begin() + frame_count * sizeof(msap1::AdcSampleFrame));
	}

	msap1::AcquisitionResponse make_response(
		const msap1::AcquisitionRequest &request) const
	{
		msap1::AcquisitionResponse response{};
		response.sequence = request.sequence;
		response.running = running_ ? 1u : 0u;
		response.published_sequence =
			__atomic_load_n(&ring_.header().published_sequence, __ATOMIC_ACQUIRE);
		response.capture_flags = ring_.header().capture_flags;
		response.iio_bytes = iio_bytes_;
		response.iio_blocks = iio_blocks_;
		response.iio_read_errors = iio_read_errors_;
		response.rpu_health = cached_health_;
		return response;
	}

	void handle_client()
	{
		const int fd = ::accept4(listen_fd_, nullptr, nullptr, SOCK_CLOEXEC);
		if (fd < 0)
			return;
		msap1::AcquisitionRequest request{};
		const auto size = ::recv(fd, &request, sizeof(request), 0);
		auto response = make_response(request);
		if (size != static_cast<ssize_t>(sizeof(request)) ||
		    request.magic != msap1::acquisition_ipc_magic ||
		    request.version != msap1::acquisition_ipc_version) {
			response.status = msap1::AcquisitionStatus::bad_request;
		} else {
			try {
				switch (request.command) {
				case msap1::AcquisitionCommand::info:
					break;
				case msap1::AcquisitionCommand::health:
					cached_health_ = query_rpu_health();
					ring_.set_capture_flags(cached_health_.capture_flags);
					break;
				case msap1::AcquisitionCommand::start:
					start();
					break;
				case msap1::AcquisitionCommand::stop:
					stop();
					break;
				default:
					response.status = msap1::AcquisitionStatus::bad_request;
					break;
				}
			} catch (const std::exception &error) {
				std::cerr << "acquisition command failed: " << error.what() << '\n';
				response.status = request.command ==
					msap1::AcquisitionCommand::health ?
					msap1::AcquisitionStatus::rpu_error :
					msap1::AcquisitionStatus::iio_error;
			}
		}
		const auto final_response = make_response(request);
		if (response.status == msap1::AcquisitionStatus::ok)
			response = final_response;
		else {
			response.running = final_response.running;
			response.published_sequence = final_response.published_sequence;
			response.iio_bytes = final_response.iio_bytes;
			response.iio_blocks = final_response.iio_blocks;
			response.iio_read_errors = final_response.iio_read_errors;
		}
		(void)::send(fd, &response, sizeof(response), MSG_NOSIGNAL);
		::close(fd);
	}

	Options options_;
	msap1::SharedRingWriter ring_;
	IioDevice iio_;
	msap1::RpmsgEndpoint endpoint_;
	int listen_fd_ = -1;
	std::uint32_t rpmsg_sequence_ = 0x90000000u;
	bool running_ = false;
	std::uint64_t iio_bytes_ = 0;
	std::uint64_t iio_blocks_ = 0;
	std::uint64_t iio_read_errors_ = 0;
	std::vector<unsigned char> pending_;
	msap1_adc_health_payload cached_health_{};
};

} // namespace

int main(int argc, char **argv)
{
	std::signal(SIGINT, handle_signal);
	std::signal(SIGTERM, handle_signal);
	try {
		AcquisitionDaemon daemon(parse_options(argc, argv));
		daemon.run();
		return 0;
	} catch (const std::exception &error) {
		std::cerr << "msap1-fpga-acquisition: " << error.what() << '\n';
		return 1;
	}
}
