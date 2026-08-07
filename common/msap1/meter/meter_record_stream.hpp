#pragma once

#include "msap1/meter/meter_record.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace msap1 {

using MeterCursor = std::uint64_t;

struct StoredMeterRecord {
	MeterCursor cursor = 0;
	std::chrono::system_clock::time_point received_at{};
	MeterRecord record{};
};

class MeterRecordStream {
public:
	explicit MeterRecordStream(
		std::filesystem::path database_path =
			"/data/mnc/meter/record-stream.sqlite3");
	~MeterRecordStream();

	MeterRecordStream(const MeterRecordStream &) = delete;
	MeterRecordStream &operator=(const MeterRecordStream &) = delete;
	MeterRecordStream(MeterRecordStream &&) noexcept;
	MeterRecordStream &operator=(MeterRecordStream &&) noexcept;

	MeterCursor append(const MeterRecord &record,
			   std::chrono::system_clock::time_point received_at =
				   std::chrono::system_clock::now());
	std::vector<StoredMeterRecord> read_after(MeterCursor cursor,
						 std::size_t limit) const;
	void register_consumer(const std::string &name);
	void acknowledge(const std::string &name, MeterCursor cursor);
	std::size_t prune(std::chrono::hours safety_window =
					std::chrono::hours{24});

	[[nodiscard]] const std::filesystem::path &path() const noexcept;

private:
	struct Impl;
	std::unique_ptr<Impl> impl_;
};

} // namespace msap1
