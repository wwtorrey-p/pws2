#pragma once
#include <string>

namespace auth {

// Loads the saved credentials, creating the default admin/admin pair on first boot.
void init();
bool check(const std::string& user, const std::string& pass);
enum class SetResult { Ok, InvalidUsername, InvalidPassword, StorageError };
SetResult set(const std::string& user, const std::string& pass);
std::string username();
bool isDefault();

}  // namespace auth
