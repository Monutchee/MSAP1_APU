#include "msap1/meter/energy_ledger.hpp"

#include "mnc/storage/sqlite/sqlite_database.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <charconv>
#include <chrono>
#include <climits>
#include <cstdio>
#include <limits>
#include <span>
#include <string_view>
#include <vector>

namespace msap1::energy_ledger {
namespace {

using mnc::storage::sqlite::Database;
using mnc::storage::sqlite::Statement;
using mnc::storage::sqlite::Transaction;

template<typename Integer, std::size_t Count>
std::vector<std::byte> encode_array(const std::array<Integer, Count> &values)
{
	static_assert(sizeof(Integer) == 8);
	std::vector<std::byte> result(Count * sizeof(Integer));
	for (std::size_t index = 0; index < Count; ++index) {
		const auto bits = std::bit_cast<std::uint64_t>(values[index]);
		for (std::size_t byte = 0; byte < 8; ++byte)
			result[index * 8 + byte] = static_cast<std::byte>(
				(bits >> (byte * 8)) & 0xffu);
	}
	return result;
}

template<typename Integer, std::size_t Count>
std::array<Integer, Count> decode_array(std::span<const std::byte> bytes,
	std::string_view name)
{
	static_assert(sizeof(Integer) == 8);
	if (bytes.size() != Count * sizeof(Integer))
		throw std::runtime_error(std::string(name) + " ledger blob size mismatch");
	std::array<Integer, Count> result{};
	for (std::size_t index = 0; index < Count; ++index) {
		std::uint64_t bits = 0;
		for (std::size_t byte = 0; byte < 8; ++byte)
			bits |= static_cast<std::uint64_t>(
				std::to_integer<std::uint8_t>(bytes[index * 8 + byte]))
				<< (byte * 8);
		result[index] = std::bit_cast<Integer>(bits);
	}
	return result;
}

template<std::size_t Count>
std::vector<std::byte> encode_u64(const std::array<std::uint64_t, Count> &values)
{
	return encode_array(values);
}

template<std::size_t Count>
std::array<std::uint64_t, Count> decode_u64(
	std::span<const std::byte> bytes, std::string_view name)
{
	return decode_array<std::uint64_t, Count>(bytes, name);
}

std::string hex64(std::uint64_t value)
{
	char result[17]{};
	(void)std::snprintf(result, sizeof result, "%016llx",
		static_cast<unsigned long long>(value));
	return result;
}

std::uint64_t parse_hex64(std::string_view value, std::string_view name)
{
	std::uint64_t result = 0;
	const auto [end, error] = std::from_chars(value.data(),
		value.data() + value.size(), result, 16);
	if (error != std::errc{} || end != value.data() + value.size())
		throw std::runtime_error(std::string(name) + " is not a uint64 hex value");
	return result;
}

template<typename Function>
void for_each_energy_reading(EnergyValues &values, Function function)
{
	const auto visit = [&function](auto &group) {
		function(group.phase_a);
		function(group.phase_b);
		function(group.phase_c);
		function(group.total);
	};
	visit(values.active_import);
	visit(values.active_export);
	visit(values.apparent);
	for (auto &quadrant : values.reactive_quadrants)
		visit(quadrant);
}

template<typename Function>
void for_each_demand_reading(DemandValues &values, Function function)
{
	const auto visit = [&function](auto &group) {
		function(group.phase_a);
		function(group.phase_b);
		function(group.phase_c);
		function(group.total);
	};
	visit(values.current_active);
	visit(values.import_peak);
	visit(values.export_peak);
}

std::vector<std::byte> energy_qualities(EnergyValues values)
{
	std::vector<std::byte> result;
	result.reserve(energy_counter_count);
	for_each_energy_reading(values, [&result](const auto &reading) {
		result.push_back(static_cast<std::byte>(reading.quality));
	});
	return result;
}

void assign_energy_qualities(EnergyValues &values,
	std::span<const std::byte> qualities)
{
	if (qualities.size() != energy_counter_count)
		throw std::runtime_error("energy quality ledger blob size mismatch");
	std::size_t index = 0;
	for_each_energy_reading(values, [&qualities, &index](auto &reading) {
		reading.quality = static_cast<MeasurementQuality>(
			std::to_integer<std::uint8_t>(qualities[index++]));
	});
}

std::vector<std::byte> demand_qualities(DemandValues values)
{
	std::vector<std::byte> result;
	result.reserve(12);
	for_each_demand_reading(values, [&result](const auto &reading) {
		result.push_back(static_cast<std::byte>(reading.quality));
	});
	return result;
}

void assign_demand_qualities(DemandValues &values,
	std::span<const std::byte> qualities)
{
	if (qualities.size() != 12)
		throw std::runtime_error("demand quality ledger blob size mismatch");
	std::size_t index = 0;
	for_each_demand_reading(values, [&qualities, &index](auto &reading) {
		reading.quality = static_cast<MeasurementQuality>(
			std::to_integer<std::uint8_t>(qualities[index++]));
	});
}

std::array<std::uint64_t, 4> flatten_samples(
	const PhaseABCTotal<std::uint64_t> &values)
{
	return {values.phase_a, values.phase_b, values.phase_c, values.total};
}

void assign_samples(PhaseABCTotal<std::uint64_t> &values,
	const std::array<std::uint64_t, 4> &source)
{
	values.phase_a = source[0];
	values.phase_b = source[1];
	values.phase_c = source[2];
	values.total = source[3];
}

std::int64_t saturating_add(std::int64_t value, std::int64_t delta,
	bool &saturated)
{
	if (delta < 0)
		throw std::logic_error("negative energy ledger delta");
	if (value > INT64_MAX - delta) {
		saturated = true;
		return INT64_MAX;
	}
	return value + delta;
}

std::int64_t demand_magnitude(std::int64_t value, bool &saturated)
{
	if (value != INT64_MIN)
		return value < 0 ? -value : value;
	saturated = true;
	return INT64_MAX;
}

std::uint32_t sample_rate(const SampleWindow &window)
{
	const auto duration = window.duration.count();
	if (window.sample_count == 0 || duration <= 0)
		return 0;
	const auto result = static_cast<std::uint64_t>(window.sample_count) *
		1000000000ULL / static_cast<std::uint64_t>(duration);
	return result > UINT32_MAX ? 0u : static_cast<std::uint32_t>(result);
}

bool same_window(const SampleWindow &left, const SampleWindow &right) noexcept
{
	return left.sample_count == right.sample_count &&
		left.duration == right.duration;
}

SystemTime system_time(std::int64_t nanoseconds)
{
	return SystemTime(std::chrono::nanoseconds(nanoseconds));
}

void validate_reset(const ResetRequest &request)
{
	if (request.idempotency_key.empty() || request.actor.empty() ||
	    request.request_id.empty())
		throw std::invalid_argument(
			"reset requires idempotency key, actor, and request ID");
}

} // namespace

struct EnergyLedger::Impl {
	struct EnergyState {
		std::uint64_t epoch = 0;
		std::uint64_t session_id = 0;
		std::uint32_t source_sequence = 0;
		std::uint32_t generation = 0;
		EnergyCounterArray session_counters{};
		EnergyCounterArray lifetime_counters{};
	};

	struct DemandState {
		std::uint64_t epoch = 0;
		std::uint64_t session_id = 0;
		std::uint32_t source_sequence = 0;
		std::uint32_t generation = 0;
		DemandValueArray current{};
		DemandValueArray raw_import{};
		DemandValueArray raw_export{};
		DemandValueArray baseline_import{};
		DemandValueArray baseline_export{};
		DemandValueArray authoritative_import{};
		DemandValueArray authoritative_export{};
		std::array<std::uint64_t, 4> import_anchors{};
		std::array<std::uint64_t, 4> export_anchors{};
	};

	explicit Impl(const std::filesystem::path &path) : database(path)
	{
		database.execute("PRAGMA journal_mode=WAL");
		database.execute("PRAGMA synchronous=FULL");
		database.execute(
			"CREATE TABLE IF NOT EXISTS energy_state("
			"id INTEGER PRIMARY KEY CHECK(id=1), epoch INTEGER NOT NULL, "
			"session_id TEXT NOT NULL, source_sequence INTEGER NOT NULL, "
			"generation INTEGER NOT NULL, session_counters BLOB NOT NULL, "
			"lifetime_counters BLOB NOT NULL, qualities BLOB NOT NULL, "
			"last_sample TEXT NOT NULL, accepted_samples TEXT NOT NULL, "
			"skipped_samples TEXT NOT NULL, accepted_blocks INTEGER NOT NULL, "
			"skipped_blocks INTEGER NOT NULL, flags INTEGER NOT NULL, "
			"sample_count INTEGER NOT NULL, sample_rate INTEGER NOT NULL, "
			"updated_ns INTEGER NOT NULL)");
		database.execute(
			"CREATE TABLE IF NOT EXISTS demand_state("
			"id INTEGER PRIMARY KEY CHECK(id=1), epoch INTEGER NOT NULL, "
			"session_id TEXT NOT NULL, source_sequence INTEGER NOT NULL, "
			"generation INTEGER NOT NULL, current_values BLOB NOT NULL, "
			"raw_import BLOB NOT NULL, raw_export BLOB NOT NULL, "
			"baseline_import BLOB NOT NULL, baseline_export BLOB NOT NULL, "
			"authoritative_import BLOB NOT NULL, authoritative_export BLOB NOT NULL, "
			"import_anchors BLOB NOT NULL, export_anchors BLOB NOT NULL, "
			"qualities BLOB NOT NULL, last_sample TEXT NOT NULL, "
			"target_sample TEXT NOT NULL, source_count INTEGER NOT NULL, "
			"source_status INTEGER NOT NULL, flags INTEGER NOT NULL, "
			"sample_count INTEGER NOT NULL, sample_rate INTEGER NOT NULL, "
			"updated_ns INTEGER NOT NULL)");
		for (const auto table : {"energy_reset_audit", "demand_reset_audit"})
			database.execute(std::string("CREATE TABLE IF NOT EXISTS ") + table +
				"(idempotency_key TEXT PRIMARY KEY, request_id TEXT NOT NULL, "
				"actor TEXT NOT NULL, requested_ns INTEGER NOT NULL, "
				"previous_epoch INTEGER NOT NULL, resulting_epoch INTEGER NOT NULL)");
		database.execute(
			"CREATE TABLE IF NOT EXISTS energy_history("
			"session_id TEXT NOT NULL, target_sample TEXT NOT NULL, "
			"captured_ns INTEGER NOT NULL, "
			"epoch INTEGER NOT NULL, counters BLOB NOT NULL, qualities BLOB NOT NULL, "
			"flags INTEGER NOT NULL, PRIMARY KEY(session_id,target_sample))");
		database.execute(
			"CREATE TABLE IF NOT EXISTS demand_history("
			"session_id TEXT NOT NULL, target_sample TEXT NOT NULL, "
			"captured_ns INTEGER NOT NULL, "
			"epoch INTEGER NOT NULL, current_values BLOB NOT NULL, "
			"import_peaks BLOB NOT NULL, export_peaks BLOB NOT NULL, "
			"qualities BLOB NOT NULL, flags INTEGER NOT NULL, "
			"PRIMARY KEY(session_id,target_sample))");
		load_energy();
		load_demand();
	}

	void load_energy()
	{
		auto query = database.prepare(
			"SELECT epoch,session_id,source_sequence,generation,session_counters,"
			"lifetime_counters,qualities,last_sample,accepted_samples,skipped_samples,"
			"accepted_blocks,skipped_blocks,flags,sample_count,sample_rate,updated_ns "
			"FROM energy_state WHERE id=1");
		if (!query.step())
			return;
		energy_state.epoch = static_cast<std::uint64_t>(query.integer(0));
		energy_state.session_id = parse_hex64(query.text(1), "energy session ID");
		energy_state.source_sequence =
			static_cast<std::uint32_t>(query.integer(2));
		energy_state.generation = static_cast<std::uint32_t>(query.integer(3));
		energy_state.session_counters =
			decode_array<std::int64_t, energy_counter_count>(
			query.blob(4), "energy session counters");
		energy_state.lifetime_counters =
			decode_array<std::int64_t, energy_counter_count>(
			query.blob(5), "energy lifetime counters");
		EnergyValues values;
		assign_energy_counters(values, energy_state.lifetime_counters);
		assign_energy_qualities(values, query.blob(6));
		values.session_id = energy_state.session_id;
		values.last_sample_index = parse_hex64(query.text(7), "energy last sample");
		values.accepted_samples = parse_hex64(query.text(8), "accepted samples");
		values.skipped_samples = parse_hex64(query.text(9), "skipped samples");
		values.accepted_blocks = static_cast<std::uint32_t>(query.integer(10));
		values.skipped_blocks = static_cast<std::uint32_t>(query.integer(11));
		const auto flags = static_cast<std::uint32_t>(query.integer(12));
		values.saturated = (flags & 1u) != 0;
		values.incomplete_input = (flags & 2u) != 0;
		values.discontinuity = (flags & 4u) != 0;
		values.reset_epoch = energy_state.epoch;
		const SampleWindow window{
			static_cast<std::uint32_t>(query.integer(13)),
			std::chrono::nanoseconds(
				static_cast<std::int64_t>(query.integer(13)) * 1000000000LL /
				std::max<std::int64_t>(query.integer(14), 1))};
		const auto measured_at = system_time(query.integer(15));
		for_each_energy_reading(values, [&](auto &reading) {
			reading.source_sequence = energy_state.source_sequence;
			reading.measured_at = measured_at;
			reading.calculation_window = window;
		});
		latest_energy = std::move(values);
	}

	void load_demand()
	{
		auto query = database.prepare(
			"SELECT epoch,session_id,source_sequence,generation,current_values,"
			"raw_import,raw_export,baseline_import,baseline_export,"
			"authoritative_import,authoritative_export,import_anchors,export_anchors,"
			"qualities,last_sample,target_sample,source_count,source_status,flags,"
			"sample_count,sample_rate,updated_ns FROM demand_state WHERE id=1");
		if (!query.step())
			return;
		demand_state.epoch = static_cast<std::uint64_t>(query.integer(0));
		demand_state.session_id = parse_hex64(query.text(1), "demand session ID");
		demand_state.source_sequence =
			static_cast<std::uint32_t>(query.integer(2));
		demand_state.generation = static_cast<std::uint32_t>(query.integer(3));
		demand_state.current =
			decode_array<std::int64_t, 4>(query.blob(4), "demand current");
		demand_state.raw_import =
			decode_array<std::int64_t, 4>(query.blob(5), "raw import peaks");
		demand_state.raw_export =
			decode_array<std::int64_t, 4>(query.blob(6), "raw export peaks");
		demand_state.baseline_import =
			decode_array<std::int64_t, 4>(query.blob(7), "baseline import");
		demand_state.baseline_export =
			decode_array<std::int64_t, 4>(query.blob(8), "baseline export");
		demand_state.authoritative_import =
			decode_array<std::int64_t, 4>(query.blob(9), "import peaks");
		demand_state.authoritative_export =
			decode_array<std::int64_t, 4>(query.blob(10), "export peaks");
		demand_state.import_anchors =
			decode_u64<4>(query.blob(11), "import anchors");
		demand_state.export_anchors =
			decode_u64<4>(query.blob(12), "export anchors");
		DemandValues values;
		assign_demand_values(values.current_active, demand_state.current);
		assign_demand_values(values.import_peak,
			demand_state.authoritative_import);
		assign_demand_values(values.export_peak,
			demand_state.authoritative_export);
		assign_samples(values.import_peak_sample, demand_state.import_anchors);
		assign_samples(values.export_peak_sample, demand_state.export_anchors);
		assign_demand_qualities(values, query.blob(13));
		values.session_id = demand_state.session_id;
		values.last_sample_index = parse_hex64(query.text(14), "demand last sample");
		values.interval_target_sample = parse_hex64(query.text(15), "demand target");
		values.source_interval_count = static_cast<std::uint32_t>(query.integer(16));
		values.source_status = static_cast<std::uint32_t>(query.integer(17));
		const auto flags = static_cast<std::uint32_t>(query.integer(18));
		values.time_aligned = (flags & 1u) != 0;
		values.contaminated = (flags & 2u) != 0;
		values.boundary_valid = (flags & 4u) != 0;
		values.saturated = (flags & 8u) != 0;
		values.incomplete_input = (flags & 16u) != 0;
		values.peak_reset_epoch = demand_state.epoch;
		const SampleWindow window{
			static_cast<std::uint32_t>(query.integer(19)),
			std::chrono::nanoseconds(
				static_cast<std::int64_t>(query.integer(19)) * 1000000000LL /
				std::max<std::int64_t>(query.integer(20), 1))};
		const auto measured_at = system_time(query.integer(21));
		for_each_demand_reading(values, [&](auto &reading) {
			reading.source_sequence = demand_state.source_sequence;
			reading.measured_at = measured_at;
			reading.calculation_window = window;
		});
		latest_demand = std::move(values);
	}

	void persist_energy(const EnergyValues &values, const EnergyState &state,
		std::uint32_t sample_count, std::uint32_t sample_rate,
		std::int64_t updated_ns)
	{
		auto statement = database.prepare(
			"INSERT OR REPLACE INTO energy_state VALUES(1,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)");
		statement.bind(1, state.epoch);
		statement.bind(2, hex64(state.session_id));
		statement.bind(3, static_cast<std::int64_t>(state.source_sequence));
		statement.bind(4, static_cast<std::int64_t>(state.generation));
		statement.bind(5, encode_array(state.session_counters));
		statement.bind(6, encode_array(state.lifetime_counters));
		statement.bind(7, energy_qualities(values));
		statement.bind(8, hex64(values.last_sample_index));
		statement.bind(9, hex64(values.accepted_samples));
		statement.bind(10, hex64(values.skipped_samples));
		statement.bind(11, static_cast<std::int64_t>(values.accepted_blocks));
		statement.bind(12, static_cast<std::int64_t>(values.skipped_blocks));
		statement.bind(13, static_cast<std::int32_t>(
			static_cast<std::uint32_t>(values.saturated) |
			(static_cast<std::uint32_t>(values.incomplete_input) << 1) |
			(static_cast<std::uint32_t>(values.discontinuity) << 2)));
		statement.bind(14, static_cast<std::int64_t>(sample_count));
		statement.bind(15, static_cast<std::int64_t>(sample_rate));
		statement.bind(16, updated_ns);
		statement.execute();
	}

	void persist_demand(const DemandValues &values, const DemandState &state,
		std::uint32_t sample_count, std::uint32_t sample_rate,
		std::int64_t updated_ns)
	{
		auto statement = database.prepare(
			"INSERT OR REPLACE INTO demand_state VALUES(1,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)");
		statement.bind(1, state.epoch);
		statement.bind(2, hex64(state.session_id));
		statement.bind(3, static_cast<std::int64_t>(state.source_sequence));
		statement.bind(4, static_cast<std::int64_t>(state.generation));
		statement.bind(5, encode_array(state.current));
		statement.bind(6, encode_array(state.raw_import));
		statement.bind(7, encode_array(state.raw_export));
		statement.bind(8, encode_array(state.baseline_import));
		statement.bind(9, encode_array(state.baseline_export));
		statement.bind(10, encode_array(state.authoritative_import));
		statement.bind(11, encode_array(state.authoritative_export));
		statement.bind(12, encode_u64(state.import_anchors));
		statement.bind(13, encode_u64(state.export_anchors));
		statement.bind(14, demand_qualities(values));
		statement.bind(15, hex64(values.last_sample_index));
		statement.bind(16, hex64(values.interval_target_sample));
		statement.bind(17, static_cast<std::int64_t>(values.source_interval_count));
		statement.bind(18, static_cast<std::int64_t>(values.source_status));
		const auto flags = static_cast<std::uint32_t>(values.time_aligned) |
			(static_cast<std::uint32_t>(values.contaminated) << 1) |
			(static_cast<std::uint32_t>(values.boundary_valid) << 2) |
			(static_cast<std::uint32_t>(values.saturated) << 3) |
			(static_cast<std::uint32_t>(values.incomplete_input) << 4);
		statement.bind(19, static_cast<std::int32_t>(flags));
		statement.bind(20, static_cast<std::int64_t>(sample_count));
		statement.bind(21, static_cast<std::int64_t>(sample_rate));
		statement.bind(22, updated_ns);
		statement.execute();
	}

	Database database;
	std::optional<EnergyValues> latest_energy{};
	std::optional<DemandValues> latest_demand{};
	EnergyState energy_state{};
	DemandState demand_state{};
};

EnergyLedger::EnergyLedger(const std::filesystem::path &path)
	: impl_(std::make_unique<Impl>(path))
{
}

EnergyLedger::~EnergyLedger() = default;

EnergyValues EnergyLedger::ingest_energy(const EnergyValues &session,
	std::uint32_t source_sequence, std::uint32_t configuration_generation,
	std::int64_t ingested_at_nanoseconds)
{
	auto counters = flatten_energy_counters(session);
	for (const auto value : counters)
		if (value < 0)
			throw std::invalid_argument("negative ENERGY session counter");
	auto next = impl_->energy_state;
	bool saturated = session.saturated;
	if (impl_->latest_energy && session.session_id == next.session_id) {
		if (source_sequence < next.source_sequence)
			throw Conflict("stale ENERGY family sequence");
		if (source_sequence == next.source_sequence) {
			const auto &latest = *impl_->latest_energy;
			if (counters != next.session_counters ||
			    configuration_generation != next.generation ||
			    session.last_sample_index != latest.last_sample_index ||
			    session.accepted_samples != latest.accepted_samples ||
			    session.skipped_samples != latest.skipped_samples ||
			    session.accepted_blocks != latest.accepted_blocks ||
			    session.skipped_blocks != latest.skipped_blocks ||
			    session.incomplete_input != latest.incomplete_input ||
			    session.discontinuity != latest.discontinuity ||
			    (session.saturated && !latest.saturated) ||
			    energy_qualities(session) != energy_qualities(latest) ||
			    !same_window(
				session.active_import.phase_a.calculation_window,
				latest.active_import.phase_a.calculation_window))
				throw Conflict(
					"duplicate ENERGY sequence has different content");
			/* This may be a retry after the ledger committed but spool
			 * publication failed. Preserve the original durable timestamp and,
			 * critically, any reset baseline installed since that commit. */
			return latest;
		}
		for (std::size_t index = 0; index < counters.size(); ++index) {
			if (counters[index] < next.session_counters[index])
				throw Conflict("same-session ENERGY counter rollback");
			next.lifetime_counters[index] = saturating_add(
				next.lifetime_counters[index],
				counters[index] - next.session_counters[index], saturated);
		}
	} else {
		for (std::size_t index = 0; index < counters.size(); ++index)
			next.lifetime_counters[index] = saturating_add(
				next.lifetime_counters[index], counters[index], saturated);
	}

	EnergyValues result = session;
	assign_energy_counters(result, next.lifetime_counters);
	result.reset_epoch = next.epoch;
	result.saturated = saturated;
	next.session_id = session.session_id;
	next.source_sequence = source_sequence;
	next.generation = configuration_generation;
	next.session_counters = counters;
	Transaction transaction(impl_->database);
	impl_->persist_energy(result, next,
		session.active_import.phase_a.calculation_window.sample_count,
		sample_rate(session.active_import.phase_a.calculation_window),
		ingested_at_nanoseconds);
	transaction.commit();
	impl_->energy_state = next;
	impl_->latest_energy = result;
	return result;
}

DemandValues EnergyLedger::ingest_demand(const DemandValues &session,
	std::uint32_t source_sequence, std::uint32_t configuration_generation,
	std::int64_t ingested_at_nanoseconds)
{
	const auto current = flatten_demand_values(session.current_active);
	const auto incoming_import = flatten_demand_values(session.import_peak);
	const auto incoming_export = flatten_demand_values(session.export_peak);
	for (const auto *values : {&incoming_import, &incoming_export})
		for (const auto value : *values)
			if (value < 0)
				throw std::invalid_argument("negative DEMAND peak");
	auto next = impl_->demand_state;
	const bool same_session = impl_->latest_demand &&
		session.session_id == next.session_id;
	if (same_session && source_sequence < next.source_sequence)
		throw Conflict("stale DEMAND family sequence");
	if (same_session && source_sequence == next.source_sequence) {
		const auto &latest = *impl_->latest_demand;
		if (current != next.current || incoming_import != next.raw_import ||
		    incoming_export != next.raw_export ||
		    configuration_generation != next.generation ||
		    session.last_sample_index != latest.last_sample_index ||
		    session.interval_target_sample != latest.interval_target_sample ||
		    session.source_interval_count != latest.source_interval_count ||
		    session.source_status != latest.source_status ||
		    session.time_aligned != latest.time_aligned ||
		    session.contaminated != latest.contaminated ||
		    session.boundary_valid != latest.boundary_valid ||
		    session.incomplete_input != latest.incomplete_input ||
		    (session.saturated && !latest.saturated) ||
		    demand_qualities(session) != demand_qualities(latest) ||
		    !same_window(session.current_active.phase_a.calculation_window,
			latest.current_active.phase_a.calculation_window))
			throw Conflict("duplicate DEMAND sequence has different content");
		/* A source watermark reset must survive retry of the already consumed
		 * interval; it cannot become the first peak of the new epoch. */
		return latest;
	}
	if (!same_session) {
		next.baseline_import.fill(0);
		next.baseline_export.fill(0);
	} else {
		for (std::size_t index = 0; index < 4; ++index)
			if (incoming_import[index] < next.raw_import[index] ||
			    incoming_export[index] < next.raw_export[index])
				throw Conflict("same-session DEMAND peak rollback");
	}
	const auto incoming_import_anchors = flatten_samples(session.import_peak_sample);
	const auto incoming_export_anchors = flatten_samples(session.export_peak_sample);
	const std::array current_quality{
		session.current_active.phase_a.quality,
		session.current_active.phase_b.quality,
		session.current_active.phase_c.quality,
		session.current_active.total.quality,
	};
	bool saturated = session.saturated;
	for (std::size_t index = 0; index < 4; ++index) {
		/* A reset watermark suppresses the RPU's pre-reset session peak, but
		 * every subsequent valid completed interval must still be eligible to
		 * establish a new (possibly smaller) administrative-epoch peak. */
		if (current_quality[index] == MeasurementQuality::valid &&
		    current[index] > 0 &&
		    current[index] > next.authoritative_import[index]) {
			next.authoritative_import[index] = current[index];
			next.import_anchors[index] = session.last_sample_index;
		} else if (current_quality[index] == MeasurementQuality::valid &&
			   current[index] < 0) {
			const auto candidate = demand_magnitude(current[index], saturated);
			if (candidate > next.authoritative_export[index]) {
				next.authoritative_export[index] = candidate;
				next.export_anchors[index] = session.last_sample_index;
			}
		}
		if (incoming_import[index] > next.baseline_import[index] &&
		    incoming_import[index] > next.authoritative_import[index]) {
			next.authoritative_import[index] = incoming_import[index];
			next.import_anchors[index] = incoming_import_anchors[index];
		}
		if (incoming_export[index] > next.baseline_export[index] &&
		    incoming_export[index] > next.authoritative_export[index]) {
			next.authoritative_export[index] = incoming_export[index];
			next.export_anchors[index] = incoming_export_anchors[index];
		}
	}
	DemandValues result = session;
	assign_demand_values(result.import_peak, next.authoritative_import);
	assign_demand_values(result.export_peak, next.authoritative_export);
	assign_samples(result.import_peak_sample, next.import_anchors);
	assign_samples(result.export_peak_sample, next.export_anchors);
	result.peak_reset_epoch = next.epoch;
	result.saturated = saturated;

	next.session_id = session.session_id;
	next.source_sequence = source_sequence;
	next.generation = configuration_generation;
	next.current = current;
	next.raw_import = incoming_import;
	next.raw_export = incoming_export;
	Transaction transaction(impl_->database);
	impl_->persist_demand(result, next,
		session.current_active.phase_a.calculation_window.sample_count,
		sample_rate(session.current_active.phase_a.calculation_window),
		ingested_at_nanoseconds);
	if (result.interval_target_sample != 0) {
		if (impl_->latest_energy) {
			auto history = impl_->database.prepare(
				"INSERT OR REPLACE INTO energy_history VALUES(?,?,?,?,?,?,?)");
			history.bind(1, hex64(impl_->energy_state.session_id));
			history.bind(2, hex64(result.interval_target_sample));
			history.bind(3, ingested_at_nanoseconds);
			history.bind(4, impl_->energy_state.epoch);
			history.bind(5,
				encode_array(impl_->energy_state.lifetime_counters));
			history.bind(6, energy_qualities(*impl_->latest_energy));
			history.bind(7, static_cast<std::int32_t>(
				static_cast<std::uint32_t>(impl_->latest_energy->saturated) |
					(static_cast<std::uint32_t>(
						impl_->latest_energy->incomplete_input) << 1) |
					(static_cast<std::uint32_t>(
						impl_->latest_energy->discontinuity) << 2)));
			history.execute();
		}
		auto history = impl_->database.prepare(
			"INSERT OR REPLACE INTO demand_history VALUES(?,?,?,?,?,?,?,?,?)");
		history.bind(1, hex64(next.session_id));
		history.bind(2, hex64(result.interval_target_sample));
		history.bind(3, ingested_at_nanoseconds);
		history.bind(4, next.epoch);
		history.bind(5, encode_array(current));
		history.bind(6, encode_array(next.authoritative_import));
		history.bind(7, encode_array(next.authoritative_export));
		history.bind(8, demand_qualities(result));
		history.bind(9, static_cast<std::int32_t>(
			static_cast<std::uint32_t>(result.contaminated) |
			(static_cast<std::uint32_t>(result.incomplete_input) << 1) |
			(static_cast<std::uint32_t>(result.saturated) << 2)));
		history.execute();
	}
	transaction.commit();
	impl_->demand_state = next;
	impl_->latest_demand = result;
	return result;
}

std::optional<EnergyValues> EnergyLedger::energy() const
{
	return impl_->latest_energy;
}

std::optional<DemandValues> EnergyLedger::demand() const
{
	return impl_->latest_demand;
}

ResetResult EnergyLedger::reset_energy(const ResetRequest &request)
{
	validate_reset(request);
	{
		auto replay = impl_->database.prepare(
			"SELECT resulting_epoch FROM energy_reset_audit WHERE idempotency_key=?");
		replay.bind(1, request.idempotency_key);
		if (replay.step())
			return {static_cast<std::uint64_t>(replay.integer(0)), true};
	}
	if (!impl_->latest_energy)
		throw Unavailable("no durable ENERGY checkpoint exists");
	if (request.expected_epoch != impl_->energy_state.epoch)
		throw Conflict("ENERGY reset epoch conflict");
	auto next = impl_->energy_state;
	const auto previous = next.epoch++;
	next.lifetime_counters.fill(0);
	auto result = *impl_->latest_energy;
	assign_energy_counters(result, next.lifetime_counters);
	result.reset_epoch = next.epoch;
	Transaction transaction(impl_->database);
	impl_->persist_energy(result, next,
		result.active_import.phase_a.calculation_window.sample_count,
		sample_rate(result.active_import.phase_a.calculation_window),
		request.requested_at_nanoseconds);
	auto audit = impl_->database.prepare(
		"INSERT INTO energy_reset_audit VALUES(?,?,?,?,?,?)");
	audit.bind(1, request.idempotency_key);
	audit.bind(2, request.request_id);
	audit.bind(3, request.actor);
	audit.bind(4, request.requested_at_nanoseconds);
	audit.bind(5, previous);
	audit.bind(6, next.epoch);
	audit.execute();
	transaction.commit();
	impl_->energy_state = next;
	impl_->latest_energy = result;
	return {next.epoch, false};
}

ResetResult EnergyLedger::reset_demand_peaks(const ResetRequest &request)
{
	validate_reset(request);
	{
		auto replay = impl_->database.prepare(
			"SELECT resulting_epoch FROM demand_reset_audit WHERE idempotency_key=?");
		replay.bind(1, request.idempotency_key);
		if (replay.step())
			return {static_cast<std::uint64_t>(replay.integer(0)), true};
	}
	if (!impl_->latest_demand)
		throw Unavailable("no durable DEMAND checkpoint exists");
	if (request.expected_epoch != impl_->demand_state.epoch)
		throw Conflict("DEMAND reset epoch conflict");
	auto next = impl_->demand_state;
	const auto previous = next.epoch++;
	next.authoritative_import.fill(0);
	next.authoritative_export.fill(0);
	next.import_anchors.fill(0);
	next.export_anchors.fill(0);
	next.baseline_import = next.raw_import;
	next.baseline_export = next.raw_export;
	auto result = *impl_->latest_demand;
	assign_demand_values(result.import_peak, next.authoritative_import);
	assign_demand_values(result.export_peak, next.authoritative_export);
	assign_samples(result.import_peak_sample, next.import_anchors);
	assign_samples(result.export_peak_sample, next.export_anchors);
	result.peak_reset_epoch = next.epoch;
	Transaction transaction(impl_->database);
	impl_->persist_demand(result, next,
		result.current_active.phase_a.calculation_window.sample_count,
		sample_rate(result.current_active.phase_a.calculation_window),
		request.requested_at_nanoseconds);
	auto audit = impl_->database.prepare(
		"INSERT INTO demand_reset_audit VALUES(?,?,?,?,?,?)");
	audit.bind(1, request.idempotency_key);
	audit.bind(2, request.request_id);
	audit.bind(3, request.actor);
	audit.bind(4, request.requested_at_nanoseconds);
	audit.bind(5, previous);
	audit.bind(6, next.epoch);
	audit.execute();
	transaction.commit();
	impl_->demand_state = next;
	impl_->latest_demand = result;
	return {next.epoch, false};
}

} // namespace msap1::energy_ledger
