#include "msap1/modbus/modbus_register_map.hpp"
#include "msap1/modbus/register_map/register_map_export.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

namespace schema_test {

using mnc::meter::MeasurementPeriod;
using mnc::meter::MeterAttributeId;
using mnc::meter::MeterAttributeKey;
using mnc::modbus::FunctionCode;
using namespace msap1::modbus::schema;

constexpr auto input = FunctionCode::read_input_registers;
constexpr RegisterBlock dense_block{input, 0x0100, 0x0020, "test.dense"};
constexpr auto dense = make_attribute_block(
	dense_block, MeasurementPeriod::Basic, DataType::float32,
	std::array{
		MeterAttributeKey{MeterAttributeId::VanRms, std::nullopt},
		MeterAttributeKey{MeterAttributeId::VbnRms, std::nullopt},
		MeterAttributeKey{MeterAttributeId::VcnRms, std::nullopt},
	});
static_assert(dense[0].address == 0x0100 && dense[1].address == 0x0102 &&
	dense[2].address == 0x0104,
	"dense register groups must advance by datatype width");

constexpr RegisterBlock indexed_block{input, 0x4000, 0x0020, "test.indexed"};
constexpr auto indexed = make_indexed_block<1, 3>(indexed_block,
	MeasurementPeriod::Basic, MeterAttributeId::VanRms, DataType::float32);
static_assert(indexed[0].address == 0x4000 && indexed[2].address == 0x4004 &&
	std::get<MeasurementSource>(indexed[0].source).attribute.index == 1 &&
	std::get<MeasurementSource>(indexed[2].source).attribute.index == 3,
	"indexed register groups must preserve address and logical indexes");

static_assert(register_width(DataType::uint16) == 1);
static_assert(register_width(DataType::uint32) == 2);
static_assert(register_width(DataType::int32) == 2);
static_assert(register_width(DataType::float32) == 2);
static_assert(register_width(DataType::uint64) == 4);
static_assert(register_width(DataType::int64) == 4);

constexpr std::array overlapping_blocks{
	RegisterBlock{input, 0x1000, 0x1000, "test.first"},
	RegisterBlock{input, 0x1800, 0x1000, "test.second"},
};
static_assert(!validate_blocks(overlapping_blocks),
	"overlapping reserved blocks must be rejected");

constexpr std::array one_block{dense_block};
constexpr std::array overlapping_entries{
	RegisterDefinition{input, 0x0100, 2, DataType::float32,
		MeasurementSource{MeasurementPeriod::Basic,
			{MeterAttributeId::VanRms, std::nullopt}}},
	RegisterDefinition{input, 0x0101, 2, DataType::float32,
		MeasurementSource{MeasurementPeriod::Basic,
			{MeterAttributeId::VbnRms, std::nullopt}}},
};
static_assert(!validate_register_map(one_block, overlapping_entries),
	"overlapping logical fields must be rejected");

constexpr RegisterBlock undersized_block{input, 0x0100, 0x0002,
	"test.undersized"};
constexpr auto escaped_entries = make_attribute_block(undersized_block,
	MeasurementPeriod::Basic, DataType::float32,
	std::array{
		MeterAttributeKey{MeterAttributeId::VanRms, std::nullopt},
		MeterAttributeKey{MeterAttributeId::VbnRms, std::nullopt},
	});
static_assert(!validate_register_map(std::array{undersized_block},
	escaped_entries), "entries escaping their reserved block must be rejected");

constexpr RegisterBlock final_register{input, 0xffff, 1, "test.last"};
constexpr std::array overflowing_entry{
	RegisterDefinition{input, 0xffff, 2, DataType::float32,
		MeasurementSource{MeasurementPeriod::Basic,
			{MeterAttributeId::VanRms, std::nullopt}}},
};
static_assert(!validate_register_map(std::array{final_register},
	overflowing_entry), "16-bit Modbus address overflow must be rejected");

constexpr std::array wrong_width{
	RegisterDefinition{input, 0x0100, 1, DataType::float32,
		MeasurementSource{MeasurementPeriod::Basic,
			{MeterAttributeId::VanRms, std::nullopt}}},
};
static_assert(!validate_register_map(one_block, wrong_width),
	"definitions must match their datatype width");

constexpr std::array duplicate_source{
	RegisterDefinition{input, 0x0100, 2, DataType::float32,
		MeasurementSource{MeasurementPeriod::Basic,
			{MeterAttributeId::VanRms, std::nullopt}}},
	RegisterDefinition{input, 0x0104, 2, DataType::float32,
		MeasurementSource{MeasurementPeriod::Basic,
			{MeterAttributeId::VanRms, std::nullopt}}},
};
static_assert(!validate_register_map(one_block, duplicate_source),
	"accidental duplicate logical sources must be rejected");

} // namespace schema_test

void require(bool condition, const char *message)
{
	if (!condition)
		throw std::runtime_error(message);
}

class SnapshotProvider final : public mnc::meter::MeterSnapshotProvider {
public:
	std::vector<mnc::meter::MeterCapabilities> capabilities() const override
	{
		return {};
	}

	std::optional<mnc::meter::MeterSnapshot> latest(
		const mnc::meter::MeterSnapshotRequest &request) const override
	{
		++reads;
		last_request = request;
		mnc::meter::MeterSnapshot snapshot;
		snapshot.period = request.period;
		snapshot.sequence = 0x12345678;
		snapshot.configuration_generation = 0xaabbccdd;
		snapshot.energy = mnc::meter::EnergySnapshotMetadata{
			.session_id = 0xfedcba9876543210ULL,
			.reset_epoch = 7,
			.last_sample_index = 0x0102030405060708ULL,
			.accepted_samples = 1234,
			.skipped_samples = 56,
			.accepted_blocks = 78,
			.skipped_blocks = 9,
			.saturated = true,
			.incomplete_input = true,
			.discontinuity = true,
		};
		snapshot.demand = mnc::meter::DemandSnapshotMetadata{
			.session_id = 0xfedcba9876543210ULL,
			.peak_reset_epoch = 8,
			.last_sample_index = 9000,
			.interval_target_sample = 10000,
			.source_interval_count = 3000,
			.source_status = 0x55aa,
			.import_peak_samples = {11, 12, 13, 14},
			.export_peak_samples = {21, 22, 23, 24},
			.time_aligned = true,
			.contaminated = true,
			.boundary_valid = true,
		};
		for (const auto &attribute : request.attributes) {
			mnc::meter::MeterAttributeValue value;
			value.attribute = attribute;
			value.quality = attribute.id == mnc::meter::MeterAttributeId::VbnRms
				? mnc::meter::ReadingQuality::Unavailable
				: mnc::meter::ReadingQuality::Valid;
			const auto descriptor = mnc::meter::describe(attribute);
			value.unit = descriptor.unit;
			switch (descriptor.unit) {
			case mnc::meter::MeterUnit::MilliHertz:
				value.value = 60'000;
				break;
			case mnc::meter::MeterUnit::MicroVolts:
				value.value = 120'000'000;
				break;
			case mnc::meter::MeterUnit::MicroAmperes:
				value.value = 5'000'000;
				break;
			case mnc::meter::MeterUnit::MicroWattHours:
			case mnc::meter::MeterUnit::MicroVarHours:
			case mnc::meter::MeterUnit::MicroVoltAmpereHours:
				value.value = 0x0123456789abcdefLL;
				break;
			case mnc::meter::MeterUnit::MicroWatts:
				value.value = attribute.id >=
					mnc::meter::MeterAttributeId::CurrentActiveDemandA &&
					attribute.id <= mnc::meter::MeterAttributeId::CurrentActiveDemandTotal
					? -2 : 0x0123456789abcdefLL;
				break;
			case mnc::meter::MeterUnit::Picowatts:
			case mnc::meter::MeterUnit::PicoVoltAmperes:
			case mnc::meter::MeterUnit::PowerFactorMillionths:
			case mnc::meter::MeterUnit::Picovars:
			case mnc::meter::MeterUnit::Millidegrees:
			case mnc::meter::MeterUnit::RatioMillionths:
				value.value = 1;
				break;
			}
			snapshot.values.push_back(value);
		}
		return snapshot;
	}

	mnc::meter::LatestSubscription subscribe_latest(
		const mnc::meter::MeterSnapshotRequest &, Callback) override
	{
		throw std::logic_error("unused in this test");
	}

	mutable std::uint32_t reads = 0;
	mutable mnc::meter::MeterSnapshotRequest last_request;
};

float decode_float(std::uint16_t high, std::uint16_t low)
{
	return std::bit_cast<float>((static_cast<std::uint32_t>(high) << 16u) |
		low);
}

void coherent_input_block()
{
	SnapshotProvider provider;
	msap1::modbus::Msap1RegisterBank registers(provider);
	const auto result = registers.read(
		mnc::modbus::FunctionCode::read_input_registers, 0, 22);
	require(result.exception == mnc::modbus::ExceptionCode::none,
		"contiguous input block failed");
	require(provider.reads == 1,
		"one Modbus request acquired more than one snapshot");
	require(decode_float(result.values[0], result.values[1]) == 60.0f,
		"frequency engineering conversion changed");
	require(decode_float(result.values[2], result.values[3]) == 120.0f,
		"voltage engineering conversion changed");
	require(std::isnan(decode_float(result.values[4], result.values[5])),
		"unavailable reading was not encoded as NaN");
	require(result.values[16] == 0xfbu,
		"quality mask no longer identifies unavailable Vbn");
	require(result.values[18] == 0x1234 && result.values[19] == 0x5678,
		"source sequence word order changed");
	require(result.values[20] == 0xaabb && result.values[21] == 0xccdd,
		"configuration generation word order changed");
}

void metadata_and_boundaries()
{
	SnapshotProvider provider;
	msap1::modbus::Msap1RegisterBank registers(provider);
	const auto metadata = registers.read(
		mnc::modbus::FunctionCode::read_holding_registers, 0, 3);
	require(metadata.exception == mnc::modbus::ExceptionCode::none &&
		metadata.values == (std::vector<std::uint16_t>{2, 0x1234, 48}),
		"register-map metadata changed");
	require(provider.reads == 0,
		"metadata read unnecessarily queried meter data");
	require(registers.read(mnc::modbus::FunctionCode::read_input_registers, 21, 2).exception ==
		mnc::modbus::ExceptionCode::illegal_data_address,
		"register boundary was not rejected");
	require(registers.read(mnc::modbus::FunctionCode::read_input_registers,
		0x20, 1).exception ==
		mnc::modbus::ExceptionCode::illegal_data_address,
		"undefined address inside a reserved block was not rejected");
	require(provider.reads == 0,
		"invalid address queried the snapshot provider before validation");
	require(registers.read(mnc::modbus::FunctionCode::read_input_registers,
		0xffff, 2).exception ==
		mnc::modbus::ExceptionCode::illegal_data_address,
		"overflowing register range did not return Illegal Data Address");
	require(provider.reads == 0,
		"overflowing address queried the snapshot provider");
	require(registers.read(mnc::modbus::FunctionCode::read_input_registers,
		0, 0).exception == mnc::modbus::ExceptionCode::illegal_data_value,
		"zero register count did not return Illegal Data Value");
	require(provider.reads == 0,
		"zero-count request queried the snapshot provider");
	require(registers.write_single(0, 1) ==
		mnc::modbus::ExceptionCode::illegal_data_address,
		"read-only initial map accepted a write");
}

void m17_energy_demand_map()
{
	SnapshotProvider provider;
	msap1::modbus::Msap1RegisterBank registers(provider);
	const auto energy = registers.read(
		mnc::modbus::FunctionCode::read_input_registers, 0x0100, 4);
	require(energy.exception == mnc::modbus::ExceptionCode::none &&
		energy.values == (std::vector<std::uint16_t>{
			0x0123, 0x4567, 0x89ab, 0xcdef}),
		"energy uint64 is not high-word-first at 0x0100");
	const auto quadrant_iv = registers.read(
		mnc::modbus::FunctionCode::read_input_registers, 0x016c, 4);
	require(quadrant_iv.exception == mnc::modbus::ExceptionCode::none,
		"quadrant-IV total is missing from 0x016c..0x016f");
	const auto energy_session = registers.read(
		mnc::modbus::FunctionCode::read_input_registers, 0x0170, 4);
	require(energy_session.values == (std::vector<std::uint16_t>{
		0xfedc, 0xba98, 0x7654, 0x3210}),
		"energy session metadata word order changed");
	const auto energy_quality = registers.read(
		mnc::modbus::FunctionCode::read_input_registers, 0x018a, 4);
	require(energy_quality.values == (std::vector<std::uint16_t>{
		0x0000, 0x0000, 0x0fff, 0xffff}),
		"28-bit energy quality mask changed");

	const auto demand = registers.read(
		mnc::modbus::FunctionCode::read_input_registers, 0x2000, 4);
	require(demand.exception == mnc::modbus::ExceptionCode::none &&
		demand.values == (std::vector<std::uint16_t>{
			0xffff, 0xffff, 0xffff, 0xfffe}),
		"signed demand int64 is not two's-complement high-word-first");
	const auto export_peak = registers.read(
		mnc::modbus::FunctionCode::read_input_registers, 0x202c, 4);
	require(export_peak.exception == mnc::modbus::ExceptionCode::none,
		"export-demand total is missing from 0x202c..0x202f");
	const auto demand_session = registers.read(
		mnc::modbus::FunctionCode::read_input_registers, 0x2030, 4);
	require(demand_session.values == energy_session.values,
		"demand session metadata word order changed");
	const auto import_anchor = registers.read(
		mnc::modbus::FunctionCode::read_input_registers, 0x2048, 4);
	require(import_anchor.values == (std::vector<std::uint16_t>{0, 0, 0, 11}),
		"demand peak sample anchors are missing");
}

void projected_snapshot_requests()
{
	SnapshotProvider provider;
	msap1::modbus::Msap1RegisterBank registers(provider);
	const auto frequency = registers.read(
		mnc::modbus::FunctionCode::read_input_registers, 0, 2);
	require(frequency.exception == mnc::modbus::ExceptionCode::none &&
		provider.last_request.attributes.size() == 1 &&
		provider.last_request.attributes.front().id ==
			mnc::meter::MeterAttributeId::Frequency,
		"single-value read did not project only its required attribute");

	const auto quality = registers.read(
		mnc::modbus::FunctionCode::read_input_registers, 16, 1);
	require(quality.exception == mnc::modbus::ExceptionCode::none &&
		provider.last_request.attributes.size() == 8,
		"quality mask did not request all published attributes coherently");
}

void generated_map_lookup_and_export()
{
	const auto definitions =
		msap1::modbus::Msap1RegisterBank::definitions();
	for (const auto &entry : definitions) {
		for (std::uint32_t offset = 0; offset < entry.words; ++offset) {
			const auto *found = msap1::modbus::schema::find_definition(
				definitions, entry.function,
				static_cast<std::uint16_t>(entry.address + offset));
			require(found == &entry,
				"binary lookup did not return the generated definition");
		}
	}
	require(msap1::modbus::schema::find_definition(definitions,
		mnc::modbus::FunctionCode::read_input_registers, 0x20) == nullptr,
		"binary lookup mapped an undefined reserved address");

	const auto csv = msap1::modbus::export_register_map(
		msap1::modbus::RegisterMapFormat::csv);
	require(static_cast<std::size_t>(std::ranges::count(csv, '\n')) ==
		definitions.size() + 1,
		"CSV exporter did not visit every generated definition exactly once");
	const auto markdown = msap1::modbus::export_register_map(
		msap1::modbus::RegisterMapFormat::markdown);
	require(markdown.find("Reserved address blocks") != std::string::npos &&
		markdown.find("voltage_harmonics") != std::string::npos,
		"Markdown exporter omitted the generated map or reserved blocks");
	const auto json = msap1::modbus::export_register_map(
		msap1::modbus::RegisterMapFormat::json);
	require(json.find("\"schema\": \"mnc.modbus-register-map.v1\"") !=
			std::string::npos &&
		json.find("\"function_code\": 3") != std::string::npos &&
		json.find("\"function_code\": 4") != std::string::npos &&
		json.find("\"source_kind\": \"measurement\"") !=
			std::string::npos &&
		json.find("\"source_kind\": \"special\"") != std::string::npos &&
		json.find("\"blocks\": [") != std::string::npos &&
		json.find("\"name\": \"voltage_harmonics\"") !=
			std::string::npos,
		"JSON exporter omitted its schema, functions, sources, or blocks");
}

void partial_register_reads()
{
	SnapshotProvider provider;
	msap1::modbus::Msap1RegisterBank registers(provider);
	const auto full_float = registers.read(
		mnc::modbus::FunctionCode::read_input_registers, 0, 6);
	require(full_float.exception == mnc::modbus::ExceptionCode::none &&
		full_float.values.size() == 6,
		"full float register fixture failed");

	const auto low_frequency = registers.read(
		mnc::modbus::FunctionCode::read_input_registers, 1, 1);
	const auto high_frequency = registers.read(
		mnc::modbus::FunctionCode::read_input_registers, 0, 1);
	require(high_frequency.exception == mnc::modbus::ExceptionCode::none &&
		high_frequency.values ==
			(std::vector<std::uint16_t>{full_float.values[0]}),
		"single high word of float32 was not sliced correctly");
	require(low_frequency.exception == mnc::modbus::ExceptionCode::none &&
		low_frequency.values ==
			(std::vector<std::uint16_t>{full_float.values[1]}),
		"single low word of float32 was not sliced correctly");
	const auto complete_frequency = registers.read(
		mnc::modbus::FunctionCode::read_input_registers, 0, 2);
	require(complete_frequency.exception == mnc::modbus::ExceptionCode::none &&
		complete_frequency.values == (std::vector<std::uint16_t>{
			full_float.values[0], full_float.values[1]}),
		"complete float32 field was not returned unchanged");
	const auto spanning_float = registers.read(
		mnc::modbus::FunctionCode::read_input_registers, 1, 2);
	require(spanning_float.exception == mnc::modbus::ExceptionCode::none &&
		spanning_float.values == (std::vector<std::uint16_t>{
			full_float.values[1], full_float.values[2]}),
		"partial read spanning float32 fields was not sliced correctly");

	const auto full_u32 = registers.read(
		mnc::modbus::FunctionCode::read_input_registers, 18, 4);
	require(full_u32.exception == mnc::modbus::ExceptionCode::none &&
		full_u32.values.size() == 4,
		"full uint32 register fixture failed");
	const auto high_sequence = registers.read(
		mnc::modbus::FunctionCode::read_input_registers, 18, 1);
	require(high_sequence.exception == mnc::modbus::ExceptionCode::none &&
		high_sequence.values ==
			(std::vector<std::uint16_t>{full_u32.values[0]}),
		"single high word of uint32 was not sliced correctly");
	const auto low_sequence = registers.read(
		mnc::modbus::FunctionCode::read_input_registers, 19, 1);
	require(low_sequence.exception == mnc::modbus::ExceptionCode::none &&
		low_sequence.values ==
			(std::vector<std::uint16_t>{full_u32.values[1]}),
		"single low word of uint32 was not sliced correctly");
	const auto complete_sequence = registers.read(
		mnc::modbus::FunctionCode::read_input_registers, 18, 2);
	require(complete_sequence.exception == mnc::modbus::ExceptionCode::none &&
		complete_sequence.values == (std::vector<std::uint16_t>{
			full_u32.values[0], full_u32.values[1]}),
		"complete uint32 field was not returned unchanged");
	const auto spanning_u32 = registers.read(
		mnc::modbus::FunctionCode::read_input_registers, 19, 2);
	require(spanning_u32.exception == mnc::modbus::ExceptionCode::none &&
		spanning_u32.values == (std::vector<std::uint16_t>{
			full_u32.values[1], full_u32.values[2]}),
		"partial read spanning uint32 fields was not sliced correctly");

	/* Each request, including a partial one, must still be coherent and use
	 * exactly one snapshot rather than reading once per register definition. */
	require(provider.reads == 10,
		"partial register reads did not use one coherent snapshot each");
}

} // namespace

int main()
{
	coherent_input_block();
	metadata_and_boundaries();
	partial_register_reads();
	projected_snapshot_requests();
	m17_energy_demand_map();
	generated_map_lookup_and_export();
}
