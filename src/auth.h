#pragma once
#include <string>

namespace auth {

// Loads the saved credentials, creating the default admin/admin pair on first boot.
void init();
bool check(const std::string& user, const std::string& pass);
// Rejects passwords under 6 characters or "admin".
bool set(const std::string& pass);
bool isDefault();

}  // namespace auth
