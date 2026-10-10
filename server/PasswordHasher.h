#ifndef PASSWORD_HASHER_H
#define PASSWORD_HASHER_H

#include <string>

namespace PasswordHasher {
std::string hashPassword(const std::string& password);
bool verifyPassword(const std::string& password, const std::string& storedValue);
std::string createSessionToken();
}

#endif
