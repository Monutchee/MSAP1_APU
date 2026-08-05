#include "msap1/settings.hpp"

#include <glaze/glaze.hpp>
#include <openssl/sha.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <fstream>
#include <iomanip>
#include <iterator>
#include <sstream>
#include <stdexcept>
#include <utility>

#include <cerrno>
#include <cstring>
#include <fcntl.h>
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

struct PendingDocument {
	std::uint64_t previous_revision = 0;
	std::uint64_t draft_generation = 0;
	std::string transaction_id;
	std::string message;
	ProductSettings previous;
	ProductSettings candidate;
};

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
std::string encode_document(const T &document, bool pretty = true)
{
	const auto encoded = pretty
		? glz::write<glz::opts{.prettify = true}>(document)
		: glz::write_json(document);
	if (!encoded)
		throw std::runtime_error("cannot encode settings JSON");
	return *encoded + "\n";
}

std::string transaction_id(std::string_view hash, std::uint64_t revision)
{
	return "settings-" + std::to_string(revision) + "-" +
		std::string(hash.substr(0, 12));
}

} // namespace

ProductSettings SettingsCodec::decode(std::string_view json)
{
	return SettingsMigrator::current(
		decode_document<ProductSettings>(json, "product settings"));
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

void SettingsValidator::validate(const ProductSettings &settings)
{
	if (settings.schema_version != 1u)
		throw std::runtime_error("unsupported settings schema version");
	if (settings.system.retained_revisions == 0u ||
	    settings.system.retained_revisions > 1000u)
		throw std::runtime_error("retained revision count must be 1..1000");
	if (!supported_adc_sample_rate(settings.metering.sample_rate_hz))
		throw std::runtime_error("unsupported persistent ADC sample rate");
	if (settings.metering.rms.window_ms == 0u ||
	    settings.metering.rms.window_ms > 10000u)
		throw std::runtime_error("RMS window must be 1..10000 ms");
	if (settings.waveform.default_pretrigger_ms > 120000u ||
	    settings.waveform.default_posttrigger_ms > 120000u)
		throw std::runtime_error("waveform defaults must not exceed 120 seconds");
	(void)prepare_meter_configuration(to_meter_configuration(settings),
		settings.metering.sample_rate_hz);
}

ProductSettings SettingsMigrator::current(ProductSettings settings)
{
	if (settings.schema_version != 1u)
		throw std::runtime_error("no migration exists for settings schema " +
			std::to_string(settings.schema_version));
	return settings;
}

MeterConversionFile to_meter_configuration(const ProductSettings &settings)
{
	MeterConversionFile result;
	result.schema_version = 3;
	result.profile_id = settings.metering.conversion.profile_id;
	result.adc_source = settings.adc.source;
	result.rms_window_ms = settings.metering.rms.window_ms;
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

AcquisitionSettingsApplier::AcquisitionSettingsApplier(Apply apply)
	: apply_(std::move(apply))
{
}

std::string_view AcquisitionSettingsApplier::name() const noexcept
{
	return "acquisition";
}

bool AcquisitionSettingsApplier::handles(
	const mnc::settings::SettingsPath &path) const noexcept
{
	return std::string_view(path.value).starts_with("$.metering") ||
	       std::string_view(path.value).starts_with("$.adc");
}

mnc::settings::ValidationResult AcquisitionSettingsApplier::validate(
	const mnc::settings::SettingsChangeSet &changes) const
{
	return {true, {}, changes.affects("$.metering") || changes.affects("$.adc")
		? mnc::settings::ApplyImpact::live
		: mnc::settings::ApplyImpact::none};
}

mnc::settings::PrepareResult AcquisitionSettingsApplier::prepare(
	const mnc::settings::SettingsApplyContext &context)
{
	auto candidate = SettingsCodec::decode(context.candidate_json);
	SettingsValidator::validate(candidate);
	return {true, {}};
}

mnc::settings::ApplyResult AcquisitionSettingsApplier::apply(
	const mnc::settings::SettingsApplyContext &context)
{
	if (apply_)
		apply_(SettingsCodec::decode(context.candidate_json));
	return {true, {}};
}

mnc::settings::HealthResult AcquisitionSettingsApplier::verify(
	const mnc::settings::SettingsApplyContext &)
{
	/* The acquisition configuration request does not return until RPU and PL
	 * generation/readback verification succeeds. */
	return {true, {}};
}

void AcquisitionSettingsApplier::rollback(
	const mnc::settings::SettingsApplyContext &context) noexcept
{
	if (!apply_)
		return;
	try {
		apply_(SettingsCodec::decode(context.previous_json));
	} catch (...) {
	}
}

WaveformSettingsApplier::WaveformSettingsApplier(Apply apply)
	: apply_(std::move(apply))
{
}

std::string_view WaveformSettingsApplier::name() const noexcept
{
	return "waveform";
}

bool WaveformSettingsApplier::handles(
	const mnc::settings::SettingsPath &path) const noexcept
{
	return std::string_view(path.value).starts_with("$.waveform");
}

mnc::settings::ValidationResult WaveformSettingsApplier::validate(
	const mnc::settings::SettingsChangeSet &changes) const
{
	return {true, {}, changes.affects("$.waveform")
		? mnc::settings::ApplyImpact::live
		: mnc::settings::ApplyImpact::none};
}

mnc::settings::PrepareResult WaveformSettingsApplier::prepare(
	const mnc::settings::SettingsApplyContext &context)
{
	auto candidate = SettingsCodec::decode(context.candidate_json);
	SettingsValidator::validate(candidate);
	return {true, {}};
}

mnc::settings::ApplyResult WaveformSettingsApplier::apply(
	const mnc::settings::SettingsApplyContext &context)
{
	if (apply_)
		apply_(SettingsCodec::decode(context.candidate_json));
	return {true, {}};
}

mnc::settings::HealthResult WaveformSettingsApplier::verify(
	const mnc::settings::SettingsApplyContext &)
{
	return {true, {}};
}

void WaveformSettingsApplier::rollback(
	const mnc::settings::SettingsApplyContext &context) noexcept
{
	if (!apply_)
		return;
	try {
		apply_(SettingsCodec::decode(context.previous_json));
	} catch (...) {
	}
}

void SettingsApplyCoordinator::apply(const ProductSettings &candidate,
				     const ProductSettings &previous) const
{
	const auto previous_json = SettingsCodec::encode(previous);
	const auto candidate_json = SettingsCodec::encode(candidate);
	mnc::settings::SettingsChangeSet changes{
		mnc::settings::JsonDiff::compare(previous_json, candidate_json)};
	if (changes.diff.changes.empty())
		return;

	mnc::settings::SettingsApplyContext context{
		"runtime-" + SettingsCodec::hash(candidate_json).substr(0, 12),
		previous_json, candidate_json, std::move(changes)};
	AcquisitionSettingsApplier acquisition(apply_);
	WaveformSettingsApplier waveform(apply_);
	mnc::settings::SettingsApplier *applier = nullptr;
	if (context.changes.affects("$.metering") ||
	    context.changes.affects("$.adc"))
		applier = &acquisition;
	else if (context.changes.affects("$.waveform"))
		applier = &waveform;
	else
		return; /* Service-independent policy such as revision retention. */

	const auto validation = applier->validate(context.changes);
	if (!validation.accepted)
		throw std::runtime_error(std::string(applier->name()) +
			" settings validation failed: " + validation.message);
	const auto prepared = applier->prepare(context);
	if (!prepared.prepared)
		throw std::runtime_error(std::string(applier->name()) +
			" settings preparation failed: " + prepared.message);
	try {
		const auto applied = applier->apply(context);
		if (!applied.applied)
			throw std::runtime_error(std::string(applier->name()) +
				" settings apply failed: " + applied.message);
		const auto health = applier->verify(context);
		if (!health.healthy)
			throw std::runtime_error(std::string(applier->name()) +
				" settings verification failed: " + health.message);
	} catch (...) {
		applier->rollback(context);
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
		/* The caller is already preserving the previous on-disk document. */
	}
}

SettingsHandler::SettingsHandler(std::filesystem::path root,
				 std::filesystem::path factory_defaults,
				 SettingsApplyCoordinator coordinator)
	: repository_(std::move(root)), revisions_(repository_.revisions_path()),
	  secrets_(repository_.secrets_path()),
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
	bool restored_pending = false;
	if (const auto pending = repository_.read_pending()) {
		if (is_blank_document(*pending)) {
			/* An empty crash marker carries no recoverable transaction.  Treat it
			 * as an interrupted file creation, remove it, and continue with the
			 * authoritative active/default document. */
			repository_.remove_pending();
		} else {
			recover_pending_locked();
			restored_pending = true;
		}
	}
	if (!restored_pending) {
		const auto persisted = repository_.read_active();
		if (persisted && !is_blank_document(*persisted)) {
			try {
				active_.settings = SettingsCodec::decode(*persisted);
				SettingsValidator::validate(active_.settings);
				const auto canonical = SettingsCodec::encode(active_.settings);
				active_.content_hash = SettingsCodec::hash(canonical);
				const auto history = revisions_.list();
				active_.revision = 0u;
				/* A revision filename is not sufficient evidence that it is the
				 * active snapshot. Match the canonical content, newest first, so an
				 * orphan/corrupt file cannot silently move the revision number. */
				for (auto entry = history.rbegin(); entry != history.rend(); ++entry) {
					try {
						auto snapshot = SettingsCodec::decode(
							revisions_.read(entry->revision));
						const auto snapshot_json = SettingsCodec::encode(snapshot);
						if (SettingsCodec::hash(snapshot_json) ==
						    active_.content_hash) {
							active_.revision = entry->revision;
							break;
						}
					} catch (...) {
					}
				}
				if (active_.revision == 0u)
					throw std::runtime_error(
						"active settings do not match a valid revision");
			} catch (const std::exception &error) {
				/* Never overwrite a corrupt initialized configuration.  Keep the
				 * best known snapshot in memory only so diagnostics and an explicit
				 * factory reset remain available. */
				recovery_mode_ = true;
				recovery_reason_ = error.what();
				bool recovered = false;
				auto history = revisions_.list();
				for (auto entry = history.rbegin(); entry != history.rend(); ++entry) {
					try {
						auto settings = SettingsCodec::decode(
							revisions_.read(entry->revision));
						SettingsValidator::validate(settings);
						const auto canonical = SettingsCodec::encode(settings);
						active_ = {entry->revision,
							SettingsCodec::hash(canonical),
							std::move(settings)};
						recovered = true;
						break;
					} catch (...) {
					}
				}
				if (!recovered) {
					auto settings = load_factory_locked();
					SettingsValidator::validate(settings);
					const auto canonical = SettingsCodec::encode(settings);
					active_ = {0u, SettingsCodec::hash(canonical),
						std::move(settings)};
				}
			}
		} else {
			/* A missing or zero-length active document is an uninitialized
			 * settings store, not a corrupt user configuration.  Seed it from
			 * the packaged, schema-validated factory document so acquisition can
			 * start without manual recovery. */
			bootstrap_locked();
		}
	}
	if (const auto persisted = repository_.read_draft()) {
		try {
			draft_ = decode_document<DraftSnapshot>(*persisted, "settings draft");
			SettingsValidator::validate(draft_->settings);
			const auto active_json = SettingsCodec::encode(active_.settings);
			const auto draft_json = SettingsCodec::encode(draft_->settings);
			if (!recovery_mode_ && active_json == draft_json) {
				/* A crash after commit publication but before draft cleanup must
				 * not resurrect a false unsaved change. */
				draft_.reset();
				repository_.remove_draft();
			} else if (!recovery_mode_ &&
				   draft_->base_revision != active_.revision) {
				/* The settings service is the sole writer, so a base mismatch on
				 * startup can only be interrupted transaction bookkeeping. Preserve
				 * the user's candidate and rebase it onto the verified active state. */
				draft_->base_revision = active_.revision;
				++draft_->generation;
				repository_.write_draft(encode_document(*draft_));
			}
		} catch (...) {
			/* Preserve the bad file for diagnosis.  It is not authoritative and
			 * therefore must not prevent reads of a valid active snapshot. */
			draft_.reset();
		}
	}
}

void SettingsHandler::bootstrap_locked()
{
	auto defaults = load_factory_locked();
	SettingsValidator::validate(defaults);
	/* Acquisition starts after msap1-settings and consumes active.json.  First
	 * boot therefore seeds durable state without contacting a service that is
	 * intentionally not running yet.  Every later commit uses the normal
	 * prepare/apply/verify transaction through coordinator_. */
	const auto canonical = SettingsCodec::encode(defaults);
	const auto digest = SettingsCodec::hash(canonical);
	/* Reset also handles a stale revision directory left beside an empty active
	 * file.  Bootstrap always establishes one coherent revision-1 baseline. */
	revisions_.reset(digest, canonical);
	repository_.write_active(canonical);
	active_ = {1u, digest, std::move(defaults)};
}

void SettingsHandler::recover_pending_locked()
{
	const auto pending_json = repository_.read_pending();
	if (!pending_json)
		return;
	const auto pending = decode_document<PendingDocument>(
		*pending_json, "pending settings transaction");
	SettingsValidator::validate(pending.previous);
	SettingsValidator::validate(pending.candidate);
	/* Recovery runs before acquisition is started.  The coordinator normally
	 * talks to that service, so its rollback may legitimately be unavailable at
	 * this point.  Disk state is the authority during recovery; acquisition
	 * will consume the restored active document after it starts. */
	try {
		coordinator_.rollback(pending.previous);
	} catch (...) {
		/* Keep recovering the durable state.  A later health check will report
		 * any subsystem that could not be re-applied. */
	}
	const auto previous_json = SettingsCodec::encode(pending.previous);
	repository_.write_active(previous_json);
	active_ = {pending.previous_revision,
		SettingsCodec::hash(previous_json), pending.previous};
	draft_ = DraftSnapshot{pending.previous_revision,
		std::max<std::uint64_t>(1u, pending.draft_generation),
		pending.candidate};
	repository_.write_draft(encode_document(*draft_));
	repository_.remove_pending();
}

ProductSettings SettingsHandler::load_factory_locked() const
{
	return SettingsCodec::decode(read_file(factory_defaults_));
}

std::uint64_t SettingsHandler::next_revision_locked() const
{
	const auto entries = revisions_.list();
	return entries.empty() ? 1u : entries.back().revision + 1u;
}

ActiveSnapshot SettingsHandler::active() const
{
	std::scoped_lock lock(mutex_);
	return active_;
}

DraftSnapshot SettingsHandler::draft() const
{
	std::scoped_lock lock(mutex_);
	return draft_locked();
}

DraftSnapshot SettingsHandler::draft_locked() const
{
	return draft_.value_or(DraftSnapshot{active_.revision, 0u, active_.settings});
}

PatchResult SettingsHandler::patch(const SettingsPatch &patch,
				   std::uint64_t expected_draft_generation)
{
	std::scoped_lock lock(mutex_);
	if (recovery_mode_)
		throw std::runtime_error("settings authority is in recovery mode");
	const auto current = draft_locked();
	if (current.generation != expected_draft_generation)
		throw std::runtime_error("stale settings draft generation");
	SettingsValidator::validate(patch.settings);
	draft_ = DraftSnapshot{current.base_revision, current.generation + 1u,
		patch.settings};
	repository_.write_draft(encode_document(*draft_));
	return {*draft_};
}

SettingsDiff SettingsHandler::diff() const
{
	std::scoped_lock lock(mutex_);
	return mnc::settings::JsonDiff::compare(
		SettingsCodec::encode(active_.settings),
		SettingsCodec::encode(draft_locked().settings));
}

CommitTransaction SettingsHandler::commit(
	std::string_view message, std::uint64_t expected_base_revision,
	std::uint64_t expected_draft_generation, const CommitActor &actor)
{
	std::scoped_lock lock(mutex_);
	return commit_locked(message, expected_base_revision,
		expected_draft_generation, actor, false);
}

CommitTransaction SettingsHandler::commit_locked(
	std::string_view message, std::uint64_t expected_base_revision,
	std::uint64_t expected_draft_generation, const CommitActor &,
	bool reset_history)
{
	if (recovery_mode_ && !reset_history)
		throw std::runtime_error("settings authority is in recovery mode");
	if (!draft_)
		throw std::runtime_error("there is no settings draft to commit");
	if (draft_->base_revision != expected_base_revision ||
	    active_.revision != expected_base_revision ||
	    draft_->generation != expected_draft_generation)
		throw std::runtime_error("stale settings commit precondition");
	SettingsValidator::validate(draft_->settings);
	const auto candidate_json = SettingsCodec::encode(draft_->settings);
	const auto digest = SettingsCodec::hash(candidate_json);
	const auto next_revision = reset_history ? 1u : next_revision_locked();
	const auto id = transaction_id(digest, next_revision);
	const PendingDocument pending{active_.revision, draft_->generation, id,
		std::string(message), active_.settings, draft_->settings};
	repository_.write_pending(encode_document(pending));
	const auto previous = active_;
	std::optional<mnc::settings::RevisionFile> created_revision;
	bool commit_marker_cleared = false;

	try {
		coordinator_.apply(draft_->settings, active_.settings);
		if (reset_history)
			revisions_.reset(digest, candidate_json);
		else
			created_revision = revisions_.create(
				next_revision, digest, candidate_json);
		repository_.write_active(candidate_json);
		/* Removing pending.json is the durable commit marker. Before this
		 * point every failure rolls disk and hardware back to previous. */
		repository_.remove_pending();
		commit_marker_cleared = true;
		active_ = {next_revision, digest, draft_->settings};
		draft_.reset();
		/* Housekeeping must not turn an already durable commit into a reported
		 * failure. Startup reconciliation removes a stale identical draft. */
		try { repository_.remove_draft(); } catch (...) {}
		try {
			revisions_.prune(active_.settings.system.retained_revisions);
		} catch (...) {}
		return {id, true, next_revision, std::string(message)};
	} catch (...) {
		if (!commit_marker_cleared) {
			coordinator_.rollback(previous.settings);
			try {
				repository_.write_active(
					SettingsCodec::encode(previous.settings));
			} catch (...) {}
			if (created_revision) {
				try { revisions_.erase(*created_revision); } catch (...) {}
			}
			try { repository_.remove_pending(); } catch (...) {}
		}
		throw;
	}
}

void SettingsHandler::discard()
{
	std::scoped_lock lock(mutex_);
	if (recovery_mode_)
		throw std::runtime_error("settings authority is in recovery mode");
	draft_.reset();
	repository_.remove_draft();
}

std::vector<RevisionInfo> SettingsHandler::history() const
{
	std::scoped_lock lock(mutex_);
	std::vector<RevisionInfo> result;
	for (const auto &entry : revisions_.list())
		result.push_back({entry.revision, entry.hash});
	return result;
}

ProductSettings SettingsHandler::revision(std::uint64_t revision) const
{
	std::scoped_lock lock(mutex_);
	return SettingsCodec::decode(revisions_.read(revision));
}

DraftSnapshot SettingsHandler::restore_to_draft(std::uint64_t revision)
{
	std::scoped_lock lock(mutex_);
	if (recovery_mode_)
		throw std::runtime_error("settings authority is in recovery mode");
	auto settings = SettingsCodec::decode(revisions_.read(revision));
	SettingsValidator::validate(settings);
	const auto generation = draft_ ? draft_->generation + 1u : 1u;
	draft_ = DraftSnapshot{active_.revision, generation, std::move(settings)};
	repository_.write_draft(encode_document(*draft_));
	return *draft_;
}

CommitTransaction SettingsHandler::factory_reset(
	const FactoryResetConfirmation &confirmation)
{
	if (!confirmation.confirmed)
		throw std::runtime_error("factory reset confirmation is required");
	std::scoped_lock lock(mutex_);
	auto defaults = load_factory_locked();
	SettingsValidator::validate(defaults);
	const auto secret_backup = secrets_.read_document();
	draft_ = DraftSnapshot{active_.revision,
		draft_ ? draft_->generation + 1u : 1u, std::move(defaults)};
	repository_.write_draft(encode_document(*draft_));
	try {
		/* Clear credentials before publishing defaults.  If any later step
		 * fails, the opaque document is restored byte-for-byte. */
		secrets_.clear();
		const auto transaction = commit_locked("factory reset", active_.revision,
			draft_->generation, confirmation.actor, true);
		recovery_mode_ = false;
		recovery_reason_.clear();
		return transaction;
	} catch (...) {
		if (secret_backup)
			secrets_.replace(*secret_backup);
		throw;
	}
}

void SettingsHandler::set_secret_document(std::string_view canonical_json)
{
	std::scoped_lock lock(mutex_);
	if (recovery_mode_)
		throw std::runtime_error("settings authority is in recovery mode");
	/* Decode once to ensure credentials are at least valid JSON. Values are
	 * deliberately opaque to the public settings model. */
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
