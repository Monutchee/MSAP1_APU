#include "msap1/meter/meter_record_stream.hpp"

#include <chrono>
#include <exception>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>

#include <unistd.h>

namespace {

using namespace std::chrono_literals;

void require(bool condition, const char *message)
{
	if (!condition)
		throw std::runtime_error(message);
}

msap1::MeterRecord record(std::uint32_t sequence)
{
	msap1::MeterRecord result{};
	result.words[0] = msap1::meter_record_magic;
	result.words[1] = msap1::meter_periodic_format;
	result.words[2] = msap1::meter_record_size;
	result.words[3] = sequence;
	return result;
}

void durable_cursor_stream()
{
	const auto directory = std::filesystem::temp_directory_path() /
		("meter-stream-test-" + std::to_string(::getpid()));
	std::filesystem::remove_all(directory);
	const auto database = directory / "records.sqlite3";
	const auto old = std::chrono::system_clock::now() - 48h;
	msap1::MeterCursor second_cursor = 0;
	{
		msap1::MeterRecordStream stream(database);
		stream.register_consumer("historian");
		stream.register_consumer("publisher");
		const auto first = stream.append(record(10), old);
		second_cursor = stream.append(record(11), old);
		const auto third = stream.append(record(12));
		require(first == 1 && second_cursor == 2 && third == 3,
			"durable stream cursors were not ordered");
		const auto page = stream.read_after(first, 2);
		require(page.size() == 2 && page[0].cursor == second_cursor &&
			page[0].record.sequence() == 11 &&
			page[1].record.sequence() == 12,
			"cursor replay returned the wrong ordered records");
		stream.acknowledge("historian", second_cursor);
		stream.acknowledge("publisher", first);
		require(stream.prune(24h) == 1,
			"pruning ignored the slowest durable consumer ACK");
		stream.acknowledge("publisher", second_cursor);
		require(stream.prune(24h) == 1,
			"fully acknowledged old record was not pruned");
	}
	{
		msap1::MeterRecordStream reopened(database);
		const auto records = reopened.read_after(0, 10);
		require(records.size() == 1 && records.front().cursor == 3 &&
			records.front().record.sequence() == 12,
			"record stream did not survive database reopen");
		bool rejected = false;
		try {
			reopened.acknowledge("unknown", second_cursor);
		} catch (const std::invalid_argument &) {
			rejected = true;
		}
		require(rejected, "unknown durable consumer was accepted");
	}
	std::filesystem::remove_all(directory);
}

} // namespace

int main()
{
	try {
		durable_cursor_stream();
		std::cout << "meter record stream tests passed\n";
		return 0;
	} catch (const std::exception &error) {
		std::cerr << "meter record stream test failed: " << error.what()
			  << '\n';
		return 1;
	}
}
