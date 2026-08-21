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
#include <map>
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

void validate_pem_asset(std::string_view name, std::string_view contents)
{
	if (contents.find('\0') != std::string_view::npos)
		throw std::invalid_argument("MQTT TLS assets must be PEM text");
	if (name == "ca" || name == "client-certificate") {
		if (!contents.contains("-----BEGIN CERTIFICATE-----") ||
		    !contents.contains("-----END CERTIFICATE-----"))
			throw std::invalid_argument(
				"MQTT certificate asset is not PEM encoded");
		return;
	}
	static constexpr std::array key_labels{
		std::string_view{"PRIVATE KEY"},
		std::string_view{"ENCRYPTED PRIVATE KEY"},
		std::string_view{"RSA PRIVATE KEY"},
		std::string_view{"EC PRIVATE KEY"},
	};
	for (const auto label : key_labels)
		if (contents.contains("-----BEGIN " + std::string(label) + "-----") &&
		    contents.contains("-----END " + std::string(label) + "-----"))
			return;
	throw std::invalid_argument("MQTT client key is not PEM encoded");
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
	auto settings = decode_document<ProductSettings>(json, "product settings");
	// Schema 1 predates MQTT. Missing members already receive typed defaults;
	// advancing the version here provides a lossless in-memory migration and
	// the next successful save persists schema 2.
	if (settings.schema_version == 1)
		settings.schema_version = ProductSettings::supported_schema_version;
	return settings;
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
	database.validate();
	modbus.validate();
	mqtt.validate();
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
	result.power_quality = settings.metering.power_quality;
	result.simulator = settings.adc.simulator;
	return result;
}

SettingsApplyCoordinator::SettingsApplyCoordinator(Apply apply,
	Apply after_persist)
	: apply_(std::move(apply)), after_persist_(std::move(after_persist))
{
}

void SettingsApplyCoordinator::after_persist(
	const ProductSettings &candidate, const ProductSettings &previous) const
{
	if (SettingsCodec::encode(candidate, false) ==
	    SettingsCodec::encode(previous, false) || !after_persist_)
		return;
	after_persist_(candidate);
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
	if (after_persist_) {
		try {
			after_persist_(previous);
		} catch (...) {
		}
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
		/* Publish the candidate in memory before post-persistence service
		 * actions. A service started or reloaded here reads this same settings
		 * authority and must observe the new snapshot, not the previous one. */
		active_ = {digest, settings};
		coordinator_.after_persist(settings, previous.settings);
	} catch (...) {
		active_ = previous;
		try {
			repository_.write_active(
				SettingsCodec::encode(previous.settings));
		} catch (...) {
		}
		coordinator_.rollback(previous.settings);
		throw;
	}

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
	std::map<std::string, std::string> previous_assets;
	for (const auto name : {"ca", "client-certificate", "client-key"}) {
		const auto path = asset_path(name);
		if (std::filesystem::is_regular_file(path))
			previous_assets.emplace(name, read_file(path));
	}
	secrets_.clear();
	for (const auto name : {"ca", "client-certificate", "client-key"}) {
		std::error_code ignored;
		std::filesystem::remove(asset_path(name), ignored);
	}
	try {
		return save_locked(defaults, true);
	} catch (...) {
		if (previous_secrets)
			secrets_.replace(*previous_secrets);
		for (const auto &[name, contents] : previous_assets)
			mnc::settings::AtomicFileWriter::write(
				asset_path(name), contents, 0600);
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

std::string SettingsHandler::validate_secret_name(std::string_view name)
{
	if (name != "mqtt.password" && name != "mqtt.private_key_passphrase")
		throw std::invalid_argument("unknown settings secret");
	return std::string(name);
}

std::string SettingsHandler::validate_asset_name(std::string_view name)
{
	if (name != "ca" && name != "client-certificate" &&
	    name != "client-key")
		throw std::invalid_argument("unknown MQTT TLS asset");
	return std::string(name);
}

void SettingsHandler::set_secret(std::string_view name, std::string_view value)
{
	std::scoped_lock lock(mutex_);
	std::map<std::string, std::string> values;
	if (const auto document = secrets_.read_document(); document &&
	    !is_blank_document(*document))
		values = decode_document<decltype(values)>(*document, "secrets");
	values[validate_secret_name(name)] = value;
	secrets_.replace(encode_document(values, true));
}

void SettingsHandler::clear_secret(std::string_view name)
{
	std::scoped_lock lock(mutex_);
	std::map<std::string, std::string> values;
	if (const auto document = secrets_.read_document(); document &&
	    !is_blank_document(*document))
		values = decode_document<decltype(values)>(*document, "secrets");
	values.erase(validate_secret_name(name));
	if (values.empty())
		secrets_.clear();
	else
		secrets_.replace(encode_document(values, true));
}

bool SettingsHandler::has_secret(std::string_view name) const
{
	std::scoped_lock lock(mutex_);
	const auto document = secrets_.read_document();
	if (!document || is_blank_document(*document))
		return false;
	const auto values = decode_document<std::map<std::string, std::string>>(
		*document, "secrets");
	return values.contains(validate_secret_name(name));
}

std::string SettingsHandler::runtime_secret(std::string_view name) const
{
	std::scoped_lock lock(mutex_);
	const auto document = secrets_.read_document();
	if (!document || is_blank_document(*document))
		return {};
	const auto values = decode_document<std::map<std::string, std::string>>(
		*document, "secrets");
	const auto found = values.find(validate_secret_name(name));
	return found == values.end() ? std::string{} : found->second;
}

std::filesystem::path SettingsHandler::asset_path(std::string_view name) const
{
	const auto validated = validate_asset_name(name);
	const auto filename = validated == "ca" ? "ca.pem" :
		validated == "client-certificate" ? "client-certificate.pem" :
		"client-key.pem";
	return repository_.root() / "assets" / "mqtt" / filename;
}

void SettingsHandler::put_asset(std::string_view name,
	std::string_view contents)
{
	if (contents.empty() || contents.size() > 1024u * 1024u)
		throw std::invalid_argument("invalid MQTT TLS asset size");
	const auto validated = validate_asset_name(name);
	validate_pem_asset(validated, contents);
	std::scoped_lock lock(mutex_);
	mnc::settings::AtomicFileWriter::write(asset_path(validated), contents, 0600);
}

void SettingsHandler::delete_asset(std::string_view name)
{
	std::scoped_lock lock(mutex_);
	std::error_code error;
	if (!std::filesystem::remove(asset_path(name), error) && error)
		throw std::runtime_error("cannot delete MQTT TLS asset: " +
			error.message());
}

bool SettingsHandler::has_asset(std::string_view name) const
{
	std::scoped_lock lock(mutex_);
	return std::filesystem::is_regular_file(asset_path(name));
}

std::string SettingsHandler::read_asset(std::string_view name) const
{
	std::scoped_lock lock(mutex_);
	return read_file(asset_path(name));
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
