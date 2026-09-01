#include "mnc/datalogger/transfer.hpp"

#if defined(MNC_DATALOGGER_HAVE_CURL)
#include <curl/curl.h>
#endif

#include <algorithm>
#include <array>
#include <mutex>
#include <stdexcept>
#include <utility>

namespace mnc::datalogger {

class CurlTransferClient::Implementation {
public:
	Implementation();
};

#if defined(MNC_DATALOGGER_HAVE_CURL)
namespace {

class CurlGlobal final {
public:
	CurlGlobal()
	{
		if (curl_global_init(CURL_GLOBAL_DEFAULT) != CURLE_OK)
			throw std::runtime_error("cannot initialize libcurl");
	}
	~CurlGlobal() { curl_global_cleanup(); }
};

CurlGlobal &curl_global()
{
	static CurlGlobal instance;
	return instance;
}

class EasyHandle final {
public:
	EasyHandle() : value(curl_easy_init())
	{
		if (!value)
			throw std::runtime_error("cannot allocate libcurl handle");
	}
	~EasyHandle() { curl_easy_cleanup(value); }
	CURL *value;
};

class StringList final {
public:
	~StringList() { curl_slist_free_all(value); }
	void append(const std::string &item)
	{
		auto *next = curl_slist_append(value, item.c_str());
		if (!next)
			throw std::bad_alloc();
		value = next;
	}
	curl_slist *value = nullptr;
};

struct UploadCursor {
	std::string_view body;
	std::size_t offset = 0;
};

std::size_t upload(char *destination, std::size_t size,
	std::size_t count, void *context)
{
	auto &cursor = *static_cast<UploadCursor *>(context);
	const auto capacity = size * count;
	const auto remaining = cursor.body.size() - cursor.offset;
	const auto amount = std::min(capacity, remaining);
	std::copy_n(cursor.body.data() + cursor.offset, amount, destination);
	cursor.offset += amount;
	return amount;
}

std::size_t discard(char *, std::size_t size, std::size_t count, void *)
{
	return size * count;
}

void set_long(CURL *handle, CURLoption option, long value)
{
	if (curl_easy_setopt(handle, option, value) != CURLE_OK)
		throw std::runtime_error("cannot configure libcurl integer option");
}

void set_size(CURL *handle, CURLoption option, curl_off_t value)
{
	if (curl_easy_setopt(handle, option, value) != CURLE_OK)
		throw std::runtime_error("cannot configure libcurl size option");
}

void set_string(CURL *handle, CURLoption option, const char *value)
{
	if (curl_easy_setopt(handle, option, value) != CURLE_OK)
		throw std::runtime_error("cannot configure libcurl string option");
}

TransferFailure failure(CURLcode code)
{
	switch (code) {
	case CURLE_OK: return TransferFailure::None;
	case CURLE_COULDNT_RESOLVE_PROXY:
	case CURLE_COULDNT_RESOLVE_HOST:
	case CURLE_COULDNT_CONNECT:
	case CURLE_OPERATION_TIMEDOUT:
	case CURLE_SEND_ERROR:
	case CURLE_RECV_ERROR:
	case CURLE_PARTIAL_FILE:
	case CURLE_GOT_NOTHING:
	case CURLE_SSL_CONNECT_ERROR:
		return TransferFailure::Transient;
	case CURLE_LOGIN_DENIED:
		return TransferFailure::Authentication;
	case CURLE_PEER_FAILED_VERIFICATION:
	case CURLE_SSL_CACERT_BADFILE:
		return TransferFailure::Verification;
	case CURLE_UNSUPPORTED_PROTOCOL:
	case CURLE_URL_MALFORMAT:
	case CURLE_BAD_FUNCTION_ARGUMENT:
		return TransferFailure::Configuration;
	default:
		return TransferFailure::RemoteRejected;
	}
}

void configure_authentication(CURL *handle, const TransferPlan &plan)
{
	switch (plan.authentication) {
	case RuntimeAuthentication::None:
	case RuntimeAuthentication::MutualTls:
		break;
	case RuntimeAuthentication::Basic:
		set_long(handle, CURLOPT_HTTPAUTH, static_cast<long>(CURLAUTH_BASIC));
		set_string(handle, CURLOPT_USERNAME, plan.username.c_str());
		set_string(handle, CURLOPT_PASSWORD, plan.password.c_str());
		break;
	case RuntimeAuthentication::Bearer:
		set_long(handle, CURLOPT_HTTPAUTH, static_cast<long>(CURLAUTH_BEARER));
		set_string(handle, CURLOPT_XOAUTH2_BEARER, plan.bearer_token.c_str());
		break;
	case RuntimeAuthentication::Password:
		set_string(handle, CURLOPT_USERNAME, plan.username.c_str());
		set_string(handle, CURLOPT_PASSWORD, plan.password.c_str());
		if (plan.protocol == OutboundProtocol::Sftp)
			set_long(handle, CURLOPT_SSH_AUTH_TYPES,
				static_cast<long>(CURLSSH_AUTH_PASSWORD));
		break;
	case RuntimeAuthentication::PrivateKey:
		set_string(handle, CURLOPT_USERNAME, plan.username.c_str());
		set_long(handle, CURLOPT_SSH_AUTH_TYPES,
			static_cast<long>(CURLSSH_AUTH_PUBLICKEY));
		set_string(handle, CURLOPT_SSH_PRIVATE_KEYFILE,
			plan.sftp_private_key_file->c_str());
		if (!plan.private_key_passphrase.empty())
			set_string(handle, CURLOPT_KEYPASSWD,
				plan.private_key_passphrase.c_str());
		break;
	}
}

} // namespace
#endif

CurlTransferClient::Implementation::Implementation()
{
#if defined(MNC_DATALOGGER_HAVE_CURL)
	(void)curl_global();
#endif
}

CurlTransferClient::CurlTransferClient()
	: implementation_(std::make_unique<Implementation>())
{
}

CurlTransferClient::~CurlTransferClient() = default;

bool curl_transfer_available() noexcept
{
#if defined(MNC_DATALOGGER_HAVE_CURL)
	return true;
#else
	return false;
#endif
}

TransferOutcome CurlTransferClient::perform(const TransferPlan &plan)
{
#if !defined(MNC_DATALOGGER_HAVE_CURL)
	(void)plan;
	return {false, 0, TransferFailure::Configuration,
		"libcurl transport support is unavailable in this build"};
#else
	try {
		EasyHandle easy;
		auto *handle = easy.value;
		std::array<char, CURL_ERROR_SIZE> error_buffer{};
		set_string(handle, CURLOPT_URL, plan.url.c_str());
		set_long(handle, CURLOPT_NOSIGNAL, 1L);
		set_long(handle, CURLOPT_CONNECTTIMEOUT,
			static_cast<long>(plan.connect_timeout_seconds));
		set_long(handle, CURLOPT_TIMEOUT,
			static_cast<long>(plan.transfer_timeout_seconds));
		set_long(handle, CURLOPT_FOLLOWLOCATION, 0L);
		set_long(handle, CURLOPT_FAILONERROR, 0L);
		set_string(handle, CURLOPT_USERAGENT, "msap1-data-sender/1");
		if (curl_easy_setopt(handle, CURLOPT_WRITEFUNCTION, discard) != CURLE_OK ||
		    curl_easy_setopt(handle, CURLOPT_WRITEDATA, nullptr) != CURLE_OK ||
		    curl_easy_setopt(handle, CURLOPT_ERRORBUFFER,
			error_buffer.data()) != CURLE_OK)
			throw std::runtime_error("cannot configure libcurl callbacks");
		configure_authentication(handle, plan);

		if (plan.protocol == OutboundProtocol::Https) {
			set_long(handle, CURLOPT_SSL_VERIFYPEER, 1L);
			set_long(handle, CURLOPT_SSL_VERIFYHOST, 2L);
			if (!plan.use_system_ca)
				set_string(handle, CURLOPT_CAINFO, plan.ca_file->c_str());
			if (plan.client_certificate_file) {
				set_string(handle, CURLOPT_SSLCERT,
					plan.client_certificate_file->c_str());
				set_string(handle, CURLOPT_SSLKEY,
					plan.client_key_file->c_str());
				if (!plan.private_key_passphrase.empty())
					set_string(handle, CURLOPT_KEYPASSWD,
						plan.private_key_passphrase.c_str());
			}
		}
		if (plan.protocol == OutboundProtocol::Sftp)
			set_string(handle, CURLOPT_SSH_KNOWNHOSTS,
				plan.known_hosts_file->c_str());

		StringList headers;
		StringList post_commands;
		UploadCursor cursor{plan.body, 0};
		if (plan.protocol == OutboundProtocol::Http ||
		    plan.protocol == OutboundProtocol::Https) {
			for (const auto &header : plan.headers)
				headers.append(header);
			if (curl_easy_setopt(handle, CURLOPT_HTTPHEADER, headers.value) !=
			    CURLE_OK)
				throw std::runtime_error("cannot configure HTTP headers");
			set_long(handle, CURLOPT_POST, 1L);
			set_string(handle, CURLOPT_POSTFIELDS, plan.body.c_str());
			set_size(handle, CURLOPT_POSTFIELDSIZE_LARGE,
				static_cast<curl_off_t>(plan.body.size()));
		} else {
			set_long(handle, CURLOPT_UPLOAD, 1L);
			set_size(handle, CURLOPT_INFILESIZE_LARGE,
				static_cast<curl_off_t>(plan.body.size()));
			if (curl_easy_setopt(handle, CURLOPT_READFUNCTION, upload) != CURLE_OK ||
			    curl_easy_setopt(handle, CURLOPT_READDATA, &cursor) != CURLE_OK)
				throw std::runtime_error("cannot configure upload reader");
			if (plan.protocol == OutboundProtocol::Ftp) {
				post_commands.append("RNFR " + *plan.remote_temporary_path);
				post_commands.append("RNTO " + *plan.remote_final_path);
				if (plan.zero_data_probe)
					post_commands.append("DELE " + *plan.remote_final_path);
			} else {
				post_commands.append("rename " + *plan.remote_temporary_path +
					" " + *plan.remote_final_path);
				if (plan.zero_data_probe)
					post_commands.append("rm " + *plan.remote_final_path);
			}
			if (curl_easy_setopt(handle, CURLOPT_POSTQUOTE,
				post_commands.value) != CURLE_OK)
				throw std::runtime_error("cannot configure remote atomic rename");
		}

		const auto code = curl_easy_perform(handle);
		long response_code = 0;
		(void)curl_easy_getinfo(handle, CURLINFO_RESPONSE_CODE, &response_code);
		if (code == CURLE_OK)
			return {true, response_code, TransferFailure::None, {}};
		return {false, response_code, failure(code), curl_easy_strerror(code)};
	} catch (const std::exception &error) {
		return {false, 0, TransferFailure::Configuration, error.what()};
	}
#endif
}

} // namespace mnc::datalogger
