#ifndef MSAP1_ACQUISITION_METER_DMA_READER_HPP
#define MSAP1_ACQUISITION_METER_DMA_READER_HPP

#include "msap1/acquisition/dma/meter_record_source.hpp"

#include <string>

namespace msap1::acquisition {

/** RAII adapter for the Linux-owned meter DMA character device. */
class MeterDmaReader final : public MeterRecordSource {
public:
	explicit MeterDmaReader(std::string device_path);
	~MeterDmaReader() override;

	MeterDmaReader(const MeterDmaReader &) = delete;
	MeterDmaReader &operator=(const MeterDmaReader &) = delete;
	MeterDmaReader(MeterDmaReader &&) = delete;
	MeterDmaReader &operator=(MeterDmaReader &&) = delete;

	void start() override;
	void stop() noexcept override;
	[[nodiscard]] int native_handle() const noexcept override { return fd_; }
	[[nodiscard]] std::string_view name() const noexcept override
	{
		return device_path_;
	}
	[[nodiscard]] MeterRecordBatch read_available() override;

private:
	std::string device_path_;
	int fd_ = -1;
};

} // namespace msap1::acquisition

#endif
