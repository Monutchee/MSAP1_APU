#include "msap1/settings/settings.hpp"

#include <glaze/glaze.hpp>
#include <openssl/sha.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <fstream>
#include <iomanip>
#include <iterator>
#include <sstream>
#include <stdexcept>
#include <utility>

#include <sys/file.h>
#include <unistd.h>

namespace msap1::settings {
namespace {

bool is_blank_document(std::string_view document)
{
	return std::ranges::all_of(document, [](unsigned char character) {
		return std::isspace(character) != 0;
	});
}

std::string read_file(const std::filesystem::path &path)
{
	std::ifstream input(path, std::ios::binary);
	if (!input)
		throw std::runtime_error("cannot open " + path.string());
	return {std::istreambuf_iterator<char>(input),
		std::istreambuf_iterator<char>()};
}

template<class T>
T decode_document(std::string_view json, std::string_view description)
{
	T result;
	if (const auto error = glz::read<glz::opts{.error_on_unknown_keys = true}>(
		result, json))
		throw std::runtime_error(std::string("invalid ") +
			std::string(description) + ": " + glz::format_error(error, json));
	return result;
}

template<class T>
std::string encode_document(const T &document, bool pretty)
{
	const auto encoded = pretty
		? glz::write<glz::opts{.prettify = true}>(document)
		: glz::write_json(document);
	if (!encoded)
		throw std::runtime_error("cannot encode settings JSON");
	return *encoded + "\n";
}

} // namespace

ProductSettings SettingsCodec::decode(std::string_view json)
{
	return decode_document<ProductSettings>(json, "product settings");
}

std::string SettingsCodec::encode(const ProductSettings &settings, bool pretty)
{
	return encode_document(settings, pretty);
}

std::string SettingsCodec::hash(std::string_view canonical_json)
{
	std::array<unsigned char, SHA256_DIGEST_LENGTH> digest{};
	SHA256(reinterpret_cast<const unsigned char *>(canonical_json.data()),
		canonical_json.size(), digest.data());
	std::ostringstream output;
	output << std::hex << std::setfill('0');
	for (const auto byte : digest)
		output << std::setw(2) << static_cast<unsigned>(byte);
	return output.str();
}

void ProductSettings::validate() const
{
	if (schema_version != supported_schema_version)
		throw std::runtime_error("unsupported settings schema version");
	metering.validate();
	waveform.validate();
	(void)prepare_meter_configuration(to_meter_configuration(*this),
		metering.sample_rate_hz);
}

void SettingsValidator::validate(const ProductSettings &settings)
{
	settings.validate();
}

MeterConversionFile to_meter_configuration(const ProductSettings &settings)
{
	MeterConversionFile result;
	result.schema_version = 3;
	result.profile_id = settings.metering.conversion.profile_id;
	result.adc_source = settings.adc.source;
	result.rms_window_ms = settings.metering.rms.window_ms;
	result.nominal_frequency_hz = settings.metering.nominal_frequency_hz;
	result.remove_dc = settings.metering.rms.remove_dc;
	result.adc_reference_volts =
		settings.metering.conversion.adc_reference_volts;
	result.current_channels = settings.metering.conversion.current_channels;
	result.voltage_channels = settings.metering.conversion.voltage_channels;
	result.frequency = settings.metering.frequency;
	result.simulator = settings.adc.simulator;
	return result;
}

SettingsApplyCoordinator::SettingsApplyCoordinator(Apply apply)
	: apply_(std::move(apply))
{
}

void SettingsApplyCoordinator::apply(const ProductSettings &candidate,
				     const ProductSettings &previous) const
{
	if (SettingsCodec::encode(candidate, false) ==
	    SettingsCodec::encode(previous, false))
		return;
	SettingsValidator::validate(candidate);
	if (!apply_)
		return;
	try {
		apply_(candidate);
	} catch (...) {
		rollback(previous);
		throw;
	}
}

void SettingsApplyCoordinator::rollback(const ProductSettings &previous) const noexcept
{
	if (!apply_)
		return;
	try {
		apply_(previous);
	} catch (...) {
	}
}

SettingsHandler::SettingsHandler(std::filesystem::path root,
				 std::filesystem::path factory_defaults,
				 SettingsApplyCoordinator coordinator)
	: repository_(std::move(root)), secrets_(repository_.secrets_path()),
	  factory_defaults_(std::move(factory_defaults)),
	  coordinator_(std::move(coordinator))
{
}

SettingsHandler::~SettingsHandler()
{
	if (lock_fd_ >= 0) {
		(void)::flock(lock_fd_, LOCK_UN);
		(void)::close(lock_fd_);
	}
}

void SettingsHandler::initialize()
{
	std::scoped_lock lock(mutex_);
	repository_.initialize_layout();
	const auto lock_path = repository_.root() / ".lock";
	lock_fd_ = ::open(lock_path.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0600);
	if (lock_fd_ < 0 || ::flock(lock_fd_, LOCK_EX | LOCK_NB) != 0) {
		const auto message = std::strerror(errno);
		if (lock_fd_ >= 0) {
			(void)::close(lock_fd_);
			lock_fd_ = -1;
		}
		throw std::runtime_error(
			"another settings authority owns " + lock_path.string() +
			": " + message);
	}

	const auto persisted = repository_.read_active();
	if (!persisted || is_blank_document(*persisted)) {
		bootstrap_locked();
		return;
	}

	try {
		auto settings = SettingsCodec::decode(*persisted);
		SettingsValidator::validate(settings);
		const auto canonical = SettingsCodec::encode(settings);
		active_ = {SettingsCodec::hash(canonical), std::move(settings)};
	} catch (const std::exception &) {
		/* Settings are product policy rather than collected customer data. If an
		 * old schema, truncated write, or otherwise invalid document is found,
		 * replace it atomically with the packaged, validated factory document.
		 * This keeps acquisition bootable after an image/schema update. */
		bootstrap_locked();
	}
}

void SettingsHandler::bootstrap_locked()
{
	auto defaults = load_factory_locked();
	SettingsValidator::validate(defaults);
	const auto canonical = SettingsCodec::encode(defaults);
	repository_.write_active(canonical);
	active_ = {SettingsCodec::hash(canonical), std::move(defaults)};
	recovery_mode_ = false;
	recovery_reason_.clear();
}

ProductSettings SettingsHandler::load_factory_locked() const
{
	return SettingsCodec::decode(read_file(factory_defaults_));
}

ActiveSnapshot SettingsHandler::active() const
{
	std::scoped_lock lock(mutex_);
	return active_;
}

ActiveSnapshot SettingsHandler::save(const ProductSettings &settings)
{
	std::scoped_lock lock(mutex_);
	return save_locked(settings, false);
}

ActiveSnapshot SettingsHandler::save_locked(const ProductSettings &settings,
					    bool allow_recovery)
{
	if (recovery_mode_ && !allow_recovery)
		throw std::runtime_error("settings authority is in recovery mode");
	SettingsValidator::validate(settings);
	const auto canonical = SettingsCodec::encode(settings);
	const auto digest = SettingsCodec::hash(canonical);
	const auto previous = active_;

	if (!allow_recovery && digest == active_.content_hash)
		return active_;

	coordinator_.apply(settings, previous.settings);
	try {
		repository_.write_active(canonical);
	} catch (...) {
		coordinator_.rollback(previous.settings);
		throw;
	}

	active_ = {digest, settings};
	recovery_mode_ = false;
	recovery_reason_.clear();
	return active_;
}

ActiveSnapshot SettingsHandler::factory_reset(bool confirmed)
{
	if (!confirmed)
		throw std::runtime_error("factory reset requires confirmation");
	std::scoped_lock lock(mutex_);
	auto defaults = load_factory_locked();
	SettingsValidator::validate(defaults);
	const auto previous_secrets = secrets_.read_document();
	secrets_.clear();
	try {
		return save_locked(defaults, true);
	} catch (...) {
		if (previous_secrets)
			secrets_.replace(*previous_secrets);
		throw;
	}
}

void SettingsHandler::set_secret_document(std::string_view canonical_json)
{
	std::scoped_lock lock(mutex_);
	glz::generic document;
	if (const auto error = glz::read_json(document, canonical_json))
		throw std::runtime_error("invalid secrets JSON: " +
			glz::format_error(error, canonical_json));
	secrets_.replace(canonical_json);
}

bool SettingsHandler::has_secrets() const
{
	std::scoped_lock lock(mutex_);
	return secrets_.exists();
}

bool SettingsHandler::recovery_mode() const
{
	std::scoped_lock lock(mutex_);
	return recovery_mode_;
}

std::string SettingsHandler::recovery_reason() const
{
	std::scoped_lock lock(mutex_);
	return recovery_reason_;
}

} // namespace msap1::settings
