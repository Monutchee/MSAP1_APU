#include "msap1/modbus/modbus_register_map.hpp"

#include <bit>
#include <cmath>
#include <cstdint>
#include <stdexcept>
#include <vector>

namespace {

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
		mnc::meter::MeterSnapshot snapshot;
		snapshot.period = request.period;
		snapshot.sequence = 0x12345678;
		snapshot.configuration_generation = 0xaabbccdd;
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
		metadata.values == (std::vector<std::uint16_t>{1, 0x1234, 8}),
		"register-map metadata changed");
	require(provider.reads == 0,
		"metadata read unnecessarily queried meter data");
	require(registers.read(mnc::modbus::FunctionCode::read_input_registers, 21, 2).exception ==
		mnc::modbus::ExceptionCode::illegal_data_address,
		"register boundary was not rejected");
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
}
