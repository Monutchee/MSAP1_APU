#pragma once

#include <optional>
#include <string>
#include <vector>

#include <webengine/AuthProvider.hpp>
#include <webengine/TestAuthProvider.hpp>

namespace msap1::web {

// Development-only authentication policy. It deliberately exposes only the
// admin/admin account and reports itself read-only so WebEngine's optional user
// management API cannot mutate credentials. Replace this provider before
// production deployment.
class Msap1AuthProvider final : public webengine::AuthProvider {
public:
	Msap1AuthProvider();

	std::optional<webengine::Role>
	authenticate(const std::string &username,
		     const std::string &password) override;
	std::vector<webengine::UserInfo> list_users() const override;

private:
	webengine::TestAuthProvider provider_;
};

} // namespace msap1::web
