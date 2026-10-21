#ifndef PASSWORD_HASHER_H
#define PASSWORD_HASHER_H

#include <string>

// Password and security-answer hashing using PBKDF2-HMAC-SHA256 (Windows BCrypt API).
// Stored format: pbkdf2$<iterations>$<saltHex>$<hashHex>
namespace PasswordHasher {
std::string hashPassword(const std::string& password);
bool verifyPassword(const std::string& password, const std::string& storedValue);
std::string createSessionToken();
}

#endif
