#include "msap1_auth_provider.hpp"

namespace msap1::web {

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
	return provider_.authenticate(username, password);
}

std::vector<webengine::UserInfo> Msap1AuthProvider::list_users() const
{
	return provider_.list_users();
}

} // namespace msap1::web
