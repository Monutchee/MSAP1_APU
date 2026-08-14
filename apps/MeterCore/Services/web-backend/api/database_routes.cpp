/** @file database_routes.cpp Administrator database policy and health API. */

#include "response.hpp"
#include "routes.hpp"

#include "msap1/settings/settings.hpp"

#include <exception>
#include <optional>
#include <string>
#include <vector>

#include <glaze/glaze.hpp>

namespace msap1::web::api {
namespace {

struct ConsumerCursorDto {
	std::string name;
	std::uint64_t acknowledged_cursor = 0;
};

struct StreamStatusDto {
	bool healthy = true;
	bool durability = true;
	std::uint64_t oldest_cursor = 0;
	std::uint64_t newest_cursor = 0;
	std::uint64_t record_count = 0;
	std::uint64_t storage_bytes = 0;
	std::uint64_t session_start_cursor = 0;
	std::uint64_t dropped_unacknowledged_records = 0;
	std::vector<ConsumerCursorDto> consumers;
};

struct DatasetStatusDto {
	std::string dataset;
	std::string backend;
	std::uint64_t block_count = 0;
	std::uint64_t storage_bytes = 0;
	std::optional<std::int64_t> oldest_nanoseconds;
	std::optional<std::int64_t> newest_nanoseconds;
};

struct HistorianStatusDto {
	bool healthy = true;
	bool migration_in_progress = false;
	bool backfill_incomplete = false;
	std::uint64_t acknowledged_cursor = 0;
	std::uint64_t oldest_available_stream_cursor = 0;
	std::uint64_t newest_stream_cursor = 0;
	std::uint64_t lag_records = 0;
	std::uint64_t block_count = 0;
	std::uint64_t storage_bytes = 0;
	std::vector<DatasetStatusDto> datasets;
};

struct DatabaseStatusDto {
	settings::DatabaseSettings policies;
	StreamStatusDto stream;
	HistorianStatusDto historian;
};

struct DatabaseMaintenanceRequestDto {
	std::string action;
	std::vector<std::string> datasets;
	bool confirmed = false;
};

std::string database_dataset_name(mnc::meter_stream::DatabaseDataset value)
{
	using D = mnc::meter_stream::DatabaseDataset;
	switch (value) {
	case D::raw_record_spool: return "spool";
	case D::basic: return "basic";
	case D::cycles_150_180: return "cycles_150_180";
	case D::minutes_10: return "minutes_10";
	case D::hours_2: return "hours_2";
	}
	return "unknown";
}

std::string backend_name(mnc::meter_stream::StorageBackend value)
{
	return value == mnc::meter_stream::StorageBackend::memory
		? "memory" : "persistent";
}

mnc::meter_stream::DatabaseDataset historian_dataset(
	std::string_view value)
{
	using D = mnc::meter_stream::DatabaseDataset;
	if (value == "basic") return D::basic;
	if (value == "cycles_150_180") return D::cycles_150_180;
	if (value == "minutes_10") return D::minutes_10;
	if (value == "hours_2") return D::hours_2;
	throw std::invalid_argument("unknown historian dataset: " +
		std::string(value));
}

DatabaseStatusDto database_status(AppContext &app)
{
	DatabaseStatusDto result;
	result.policies = settings::SettingsCodec::decode(
		app.settings.active().json).database;

	const auto stream = app.database.stream_status();
	result.stream.durability = stream.durability;
	result.stream.oldest_cursor = stream.oldest_cursor;
	result.stream.newest_cursor = stream.newest_cursor;
	result.stream.record_count = stream.record_count;
	result.stream.storage_bytes = stream.storage_bytes;
	result.stream.session_start_cursor = stream.session_start_cursor;
	result.stream.dropped_unacknowledged_records =
		stream.dropped_unacknowledged_records;
	for (const auto &consumer : stream.consumers)
		result.stream.consumers.push_back(
			{consumer.name, consumer.acknowledged_cursor});

	const auto historian = app.database.historian_status();
	result.historian.healthy = historian.healthy;
	result.historian.migration_in_progress =
		historian.migration_in_progress;
	result.historian.backfill_incomplete = historian.backfill_incomplete;
	result.historian.acknowledged_cursor = historian.acknowledged_cursor;
	result.historian.oldest_available_stream_cursor =
		historian.oldest_available_stream_cursor;
	result.historian.newest_stream_cursor = stream.newest_cursor;
	result.historian.lag_records = stream.newest_cursor >
		historian.acknowledged_cursor
		? stream.newest_cursor - historian.acknowledged_cursor : 0;
	result.historian.block_count = historian.block_count;
	result.historian.storage_bytes = historian.storage_bytes;
	for (const auto &dataset : historian.datasets) {
		result.historian.datasets.push_back({
			database_dataset_name(dataset.dataset),
			backend_name(dataset.backend), dataset.block_count,
			dataset.storage_bytes, dataset.oldest_nanoseconds,
			dataset.newest_nanoseconds});
	}
	return result;
}

} // namespace

webengine::Response get_developer_database(
	AppContext &app, const webengine::RequestContext &)
{
	try {
		return json_response(webengine::http::status::ok,
			database_status(app));
	} catch (const std::exception &error) {
		log_api_failure("/api/v1/developer/database", error);
		return error_response(webengine::http::status::service_unavailable,
			error.what());
	}
}

webengine::Response put_developer_database(
	AppContext &app, const webengine::RequestContext &context)
{
	try {
		settings::DatabaseSettings policies;
		if (glz::read_json(policies, context.request.body()))
			return error_response(webengine::http::status::bad_request,
				"invalid database policy JSON");
		policies.validate();
		(void)app.settings.update_and_save(
			[&](settings::ProductSettings &value) {
				value.database = policies;
			}, 120000);
		return json_response(webengine::http::status::ok,
			database_status(app));
	} catch (const std::invalid_argument &error) {
		return error_response(webengine::http::status::bad_request,
			error.what());
	} catch (const std::exception &error) {
		log_api_failure("/api/v1/developer/database", error);
		return error_response(webengine::http::status::conflict,
			error.what());
	}
}

webengine::Response post_developer_database_maintenance(
	AppContext &app, const webengine::RequestContext &context)
{
	try {
		DatabaseMaintenanceRequestDto request;
		if (glz::read_json(request, context.request.body()))
			return error_response(webengine::http::status::bad_request,
				"invalid database maintenance JSON");
		if (!request.confirmed)
			return error_response(webengine::http::status::bad_request,
				"database maintenance requires explicit confirmation");

		if (request.action == "clear_datasets") {
			if (request.datasets.empty())
				throw std::invalid_argument(
					"at least one historian dataset is required");
			std::vector<mnc::meter_stream::DatabaseDataset> datasets;
			datasets.reserve(request.datasets.size());
			for (const auto &dataset : request.datasets)
				datasets.push_back(historian_dataset(dataset));
			app.database.clear_history(datasets);
		} else if (request.action == "recreate_historian") {
			if (!request.datasets.empty())
				throw std::invalid_argument(
					"recreate_historian does not accept datasets");
			app.database.recreate_history_database();
		} else {
			throw std::invalid_argument(
				"unsupported database maintenance action");
		}

		return json_response(webengine::http::status::ok,
			database_status(app));
	} catch (const std::invalid_argument &error) {
		return error_response(webengine::http::status::bad_request,
			error.what());
	} catch (const std::exception &error) {
		log_api_failure("/api/v1/developer/database/maintenance", error);
		return error_response(webengine::http::status::conflict,
			error.what());
	}
}

} // namespace msap1::web::api
