#include "msap1_auth_provider.hpp"

#include "mnc/logging/logging.hpp"

#include <array>

namespace msap1::web {
namespace {

const mnc::logging::Logger auth_log("web-backend", "auth");

} // namespace

Msap1AuthProvider::Msap1AuthProvider()
{
	for (const auto &user : provider_.list_users())
		(void)provider_.remove_user(user.username);
	(void)provider_.add_user("admin", "admin", webengine::Role::Admin);
}

std::optional<webengine::Role>
Msap1AuthProvider::authenticate(const std::string &username,
				const std::string &password)
{
	auto role = provider_.authenticate(username, password);
	if (!role)
		(void)auth_log.write(mnc::logging::Priority::warning,
			"authentication failed for user " + username,
			"authentication_failed",
			std::array<mnc::logging::Field, 1>{
				mnc::logging::Field{"MNC_USERNAME", username}});
	return role;
}

std::vector<webengine::UserInfo> Msap1AuthProvider::list_users() const
{
	return provider_.list_users();
}

} // namespace msap1::web
