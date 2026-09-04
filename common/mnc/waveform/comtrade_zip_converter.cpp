#include "mnc/waveform/comtrade_zip_converter.hpp"

#include "mnc/waveform/comtrade_converter.hpp"

#include <zlib.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <limits>
#include <string>
#include <utility>
#include <vector>

namespace mnc::waveform {
namespace {

void append_u16(std::vector<std::byte> &bytes, std::uint16_t value)
{
	bytes.push_back(static_cast<std::byte>(value & 0xffu));
	bytes.push_back(static_cast<std::byte>((value >> 8u) & 0xffu));
}

void append_u32(std::vector<std::byte> &bytes, std::uint32_t value)
{
	for (unsigned shift = 0; shift != 32; shift += 8)
		bytes.push_back(static_cast<std::byte>((value >> shift) & 0xffu));
}

std::string member_stem(std::string_view requested)
{
	std::string value;
	value.reserve(std::min<std::size_t>(requested.size(), 96));
	for (const unsigned char character : requested) {
		if (value.size() == 96)
			break;
		if (std::isalnum(character) != 0 || character == '-' || character == '_')
			value.push_back(static_cast<char>(character));
		else if (character == ' ')
			value.push_back('_');
	}
	return value.empty() ? "waveform" : value;
}

class ZipSink final : public OutputSink {
public:
	ZipSink(OutputSink &destination, std::string stem, std::uint64_t limit)
		: destination_(destination), stem_(std::move(stem)),
		  limit_(std::min(destination.byte_limit(), limit))
	{
	}

	void write(std::span<const std::byte> bytes) override
	{
		if (!prefix_seen_) {
			consume_prefix(bytes);
			return;
		}
		if (!dat_open_)
			throw ConversionError(ConversionErrorCode::validation_failed,
				"COMTRADE CFF did not contain a DAT region");
		write_member_data(bytes);
	}

	[[nodiscard]] std::uint64_t bytes_written() const noexcept override
	{
		return destination_.bytes_written();
	}
	[[nodiscard]] std::uint64_t byte_limit() const noexcept override
	{
		return limit_;
	}

	void finish()
	{
		if (!prefix_seen_ || !dat_open_ || member_size_ != expected_dat_size_)
			throw ConversionError(ConversionErrorCode::validation_failed,
				"COMTRADE DAT byte count disagrees with its CFF declaration");
		close_member();
		const auto directory_offset = checked_u32(destination_.bytes_written(),
			"ZIP central-directory offset");
		for (const auto &entry : entries_)
			write_central_entry(entry);
		const auto directory_size = checked_u32(
			destination_.bytes_written() - directory_offset,
			"ZIP central-directory size");
		std::vector<std::byte> ending;
		append_u32(ending, 0x06054b50u);
		append_u16(ending, 0u);
		append_u16(ending, 0u);
		append_u16(ending, static_cast<std::uint16_t>(entries_.size()));
		append_u16(ending, static_cast<std::uint16_t>(entries_.size()));
		append_u32(ending, directory_size);
		append_u32(ending, directory_offset);
		append_u16(ending, 0u);
		checked_write(ending);
	}

private:
	struct Entry {
		std::string name;
		std::uint32_t crc = 0;
		std::uint32_t size = 0;
		std::uint32_t offset = 0;
	};

	[[nodiscard]] static std::uint32_t checked_u32(std::uint64_t value,
		std::string_view field)
	{
		if (value > std::numeric_limits<std::uint32_t>::max())
			throw ConversionError(ConversionErrorCode::output_too_large,
				std::string(field) + " exceeds the classic ZIP limit");
		return static_cast<std::uint32_t>(value);
	}

	void checked_write(std::span<const std::byte> bytes)
	{
		if (bytes.size() > limit_ - destination_.bytes_written())
			throw ConversionError(ConversionErrorCode::output_too_large,
				"COMTRADE ZIP exceeds its configured output limit");
		destination_.write(bytes);
	}

	void open_member(std::string name)
	{
		if (name.size() > std::numeric_limits<std::uint16_t>::max())
			throw ConversionError(ConversionErrorCode::internal_error,
				"ZIP member name is too long");
		Entry entry;
		entry.name = std::move(name);
		entry.offset = checked_u32(destination_.bytes_written(),
			"ZIP local-header offset");
		entries_.push_back(std::move(entry));
		crc_ = ::crc32(0L, Z_NULL, 0);
		member_size_ = 0;
		std::vector<std::byte> header;
		append_u32(header, 0x04034b50u);
		append_u16(header, 20u);
		append_u16(header, 0x0008u); // trailing data descriptor
		append_u16(header, 0u);      // stored, no compression
		append_u16(header, 0u);      // 00:00:00
		append_u16(header, 0x0021u); // deterministic ZIP epoch, 1980-01-01
		append_u32(header, 0u);
		append_u32(header, 0u);
		append_u32(header, 0u);
		append_u16(header, static_cast<std::uint16_t>(entries_.back().name.size()));
		append_u16(header, 0u);
		header.insert(header.end(),
			reinterpret_cast<const std::byte *>(entries_.back().name.data()),
			reinterpret_cast<const std::byte *>(entries_.back().name.data() +
				entries_.back().name.size()));
		checked_write(header);
	}

	void write_member_data(std::span<const std::byte> bytes)
	{
		if (bytes.size() > std::numeric_limits<std::uint32_t>::max() - member_size_)
			throw ConversionError(ConversionErrorCode::output_too_large,
				"ZIP member exceeds the classic ZIP limit");
		checked_write(bytes);
		crc_ = ::crc32(crc_, reinterpret_cast<const Bytef *>(bytes.data()),
			static_cast<uInt>(bytes.size()));
		member_size_ += static_cast<std::uint32_t>(bytes.size());
	}

	void close_member()
	{
		auto &entry = entries_.back();
		entry.crc = static_cast<std::uint32_t>(crc_);
		entry.size = member_size_;
		std::vector<std::byte> descriptor;
		append_u32(descriptor, 0x08074b50u);
		append_u32(descriptor, entry.crc);
		append_u32(descriptor, entry.size);
		append_u32(descriptor, entry.size);
		checked_write(descriptor);
	}

	void consume_prefix(std::span<const std::byte> bytes)
	{
		const std::string text(reinterpret_cast<const char *>(bytes.data()),
			bytes.size());
		constexpr std::string_view cfg_marker = "--- file type: CFG ---\r\n";
		constexpr std::string_view inf_marker = "--- file type: INF ---\r\n";
		constexpr std::string_view dat_marker = "--- file type: DAT BINARY32: ";
		if (!text.starts_with(cfg_marker))
			throw ConversionError(ConversionErrorCode::validation_failed,
				"COMTRADE CFF CFG marker is missing");
		const auto cfg_end = text.find(inf_marker, cfg_marker.size());
		const auto dat_begin = text.rfind(dat_marker);
		if (cfg_end == std::string::npos || dat_begin == std::string::npos ||
		    dat_begin <= cfg_end)
			throw ConversionError(ConversionErrorCode::validation_failed,
				"COMTRADE CFF region framing is invalid");
		const auto digits = dat_begin + dat_marker.size();
		const auto digits_end = text.find(" ---\r\n", digits);
		if (digits_end == std::string::npos)
			throw ConversionError(ConversionErrorCode::validation_failed,
				"COMTRADE CFF DAT length is invalid");
		try {
			expected_dat_size_ = std::stoull(text.substr(digits,
				digits_end - digits));
		} catch (...) {
			throw ConversionError(ConversionErrorCode::validation_failed,
				"COMTRADE CFF DAT length is invalid");
		}
		open_member(stem_ + ".cfg");
		write_member_data(bytes.subspan(cfg_marker.size(),
			cfg_end - cfg_marker.size()));
		close_member();
		open_member(stem_ + ".dat");
		dat_open_ = true;
		prefix_seen_ = true;
	}

	void write_central_entry(const Entry &entry)
	{
		std::vector<std::byte> header;
		append_u32(header, 0x02014b50u);
		append_u16(header, 20u);
		append_u16(header, 20u);
		append_u16(header, 0x0008u);
		append_u16(header, 0u);
		append_u16(header, 0u);      // 00:00:00
		append_u16(header, 0x0021u); // deterministic ZIP epoch, 1980-01-01
		append_u32(header, entry.crc);
		append_u32(header, entry.size);
		append_u32(header, entry.size);
		append_u16(header, static_cast<std::uint16_t>(entry.name.size()));
		append_u16(header, 0u);
		append_u16(header, 0u);
		append_u16(header, 0u);
		append_u16(header, 0u);
		append_u32(header, 0u);
		append_u32(header, entry.offset);
		header.insert(header.end(),
			reinterpret_cast<const std::byte *>(entry.name.data()),
			reinterpret_cast<const std::byte *>(entry.name.data() + entry.name.size()));
		checked_write(header);
	}

	OutputSink &destination_;
	std::string stem_;
	std::uint64_t limit_;
	std::vector<Entry> entries_;
	uLong crc_ = 0;
	std::uint32_t member_size_ = 0;
	std::uint64_t expected_dat_size_ = 0;
	bool prefix_seen_ = false;
	bool dat_open_ = false;
};

} // namespace

ConversionSummary ComtradeZipConverter::convert(const WaveformSource &source,
	OutputSink &sink, const ConversionOptions &options, std::stop_token stop_token,
	ProgressCallback progress) const
{
	if (options.format != ExportFormat::comtrade_zip)
		throw ConversionError(ConversionErrorCode::invalid_options,
			"COMTRADE ZIP converter received a different output format");
	ZipSink zip(sink, member_stem(options.output_stem),
		options.maximum_output_bytes);
	ConversionOptions inner = options;
	inner.format = ExportFormat::comtrade;
	const auto cff_summary = ComtradeConverter{}.convert(source, zip, inner,
		stop_token, std::move(progress));
	(void)cff_summary;
	zip.finish();
	return {ExportFormat::comtrade_zip,
		"IEC 60255-24:2013 CFG/DAT ZIP (BINARY32)", source.frame_count(),
		sink.bytes_written(), ".zip", "application/zip"};
}

} // namespace mnc::waveform
