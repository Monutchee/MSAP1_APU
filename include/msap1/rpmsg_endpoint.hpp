#ifndef MSAP1_APU_RPMSG_ENDPOINT_HPP
#define MSAP1_APU_RPMSG_ENDPOINT_HPP

#include <chrono>
#include <cstdint>
#include <string>
#include <vector>

namespace msap1 {

class RpmsgEndpoint {
public:
	RpmsgEndpoint(std::string service, std::string device = {});
	~RpmsgEndpoint();

	RpmsgEndpoint(const RpmsgEndpoint &) = delete;
	RpmsgEndpoint &operator=(const RpmsgEndpoint &) = delete;
	RpmsgEndpoint(RpmsgEndpoint &&) = delete;
	RpmsgEndpoint &operator=(RpmsgEndpoint &&) = delete;

	void send(const std::vector<std::uint8_t> &frame) const;
	std::vector<std::uint8_t> receive(std::chrono::milliseconds timeout) const;
	const std::string &device_path() const noexcept { return device_path_; }

private:
	int data_fd_ = -1;
	int control_fd_ = -1;
	bool created_endpoint_ = false;
	std::string device_path_;
};

} // namespace msap1

#endif
