#include "mnc/ipc/ipc.hpp"
#include "msap1/datalogger/msap1_datalogger.hpp"

#include <boost/asio/io_context.hpp>

#include <array>
#include <atomic>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <thread>

#include <unistd.h>

namespace {

using mnc::datalogger::DatalogError;
using mnc::datalogger::DatalogErrorCode;
using mnc::meter::MeterAttributeId;
using mnc::meter::MeterAttributeKey;
using mnc::meter::MeasurementPeriod;
using mnc::meter_stream::DatabaseDataset;
using mnc::meter_stream::StorageBackend;
using msap1::history::ipc::Command;

void require(bool condition, const char *message)
{
	if (!condition)
		throw std::runtime_error(message);
}

class FakeHistorian final {
public:
	explicit FakeHistorian(std::filesystem::path path)
		: path_(std::move(path)), server_(context_.get_executor(), path_.string())
	{
		server_.start(
			[this](auto connection, auto frame) {
				mnc::ipc::ByteWriter output;
				output.u32(0);
				switch (static_cast<Command>(frame.message_type)) {
				case Command::get_historian_status:
					encode_status(output);
					break;
				case Command::query_history:
					++query_count;
					output.u32(0); // no points are needed for this readiness test
					break;
				default:
					throw std::runtime_error(
						"unexpected fake historian command");
				}
				frame.kind = mnc::ipc::FrameKind::response;
				frame.payload = output.take();
				connection->post_send(std::move(frame));
			},
			[this](const std::string &) { ++connection_errors; });
		worker_ = std::thread([this] { context_.run(); });
	}

	~FakeHistorian()
	{
		server_.stop();
		context_.stop();
		if (worker_.joinable())
			worker_.join();
		std::error_code ignored;
		std::filesystem::remove(path_, ignored);
	}

	std::atomic<bool> migrating{false};
	std::atomic<std::uint32_t> query_count{0};
	std::atomic<std::uint32_t> connection_errors{0};

private:
	void encode_status(mnc::ipc::ByteWriter &output) const
	{
		output.u8(1); // healthy
		output.u8(migrating ? 1 : 0);
		output.u8(1); // an older, unrecoverable backfill gap exists
		output.u8(0);
		output.u64(1000);
		output.u64(10);
		output.u64(100);
		output.u64(4096);
		output.u64(0);
		output.u32(1);
		output.u8(static_cast<std::uint8_t>(DatabaseDataset::basic));
		output.u8(static_cast<std::uint8_t>(StorageBackend::persistent));
		output.u8(1); // oldest present
		output.u8(1); // newest present
		output.u64(100);
		output.u64(4096);
		output.i64(100);
		output.i64(1000);
	}

	std::filesystem::path path_;
	boost::asio::io_context context_;
	mnc::ipc::UnixStreamServer server_;
	std::thread worker_;
};

void retained_windows_survive_an_older_backfill_gap()
{
	const auto path = std::filesystem::temp_directory_path() /
		("msap1-datalogger-test-" + std::to_string(::getpid()) + ".sock");
	FakeHistorian server(path);
	msap1::history::ipc::HistorianClient client(path.string());
	msap1::datalogger::Msap1HistorianDataSource source(client);
	const MeterAttributeKey voltage{MeterAttributeId::VanRms, std::nullopt};
	const std::array attributes{voltage};

	const auto points = source.query(MeasurementPeriod::Basic, attributes,
		{200, 300});
	require(points.empty() && server.query_count == 1,
		"retained window was blocked by an unrelated older backfill gap");

	bool retention_gap = false;
	try {
		(void)source.query(MeasurementPeriod::Basic, attributes, {50, 200});
	} catch (const DatalogError &error) {
		retention_gap = error.code() == DatalogErrorCode::SourceRetentionGap;
	}
	require(retention_gap && server.query_count == 1,
		"window crossing the reported retention boundary was not rejected");

	server.migrating = true;
	bool migration_blocked = false;
	try {
		(void)source.query(MeasurementPeriod::Basic, attributes, {200, 300});
	} catch (const DatalogError &error) {
		migration_blocked =
			error.code() == DatalogErrorCode::SourceUnavailable;
	}
	require(migration_blocked && server.query_count == 1,
		"active historian migration did not hold generation pending");
	require(server.connection_errors == 0,
		"fake historian reported an IPC connection error");
}

} // namespace

int main()
{
	try {
		retained_windows_survive_an_older_backfill_gap();
		std::cout << "MSAP1 datalogger tests passed\n";
		return 0;
	} catch (const std::exception &error) {
		std::cerr << "MSAP1 datalogger tests failed: " << error.what() << '\n';
		return 1;
	}
}
