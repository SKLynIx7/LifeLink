#include "PasswordHasher.h"

#include <windows.h>
#include <bcrypt.h>

#include <iomanip>
#include <sstream>
#include <vector>

namespace {
// A slow, salted key-derivation function makes stolen hashes expensive to crack,
// unlike a single SHA-256 pass.
constexpr ULONGLONG PBKDF2_ITERATIONS = 100000;
constexpr ULONG DERIVED_KEY_BYTES = 32;
constexpr std::size_t SALT_BYTES = 16;

std::string toHex(const std::vector<unsigned char>& bytes) {
    std::ostringstream output;
    for (unsigned char byte : bytes) {
        output << std::hex << std::setw(2) << std::setfill('0') << static_cast<int>(byte);
    }
    return output.str();
}

bool fromHex(const std::string& value, std::vector<unsigned char>& bytes) {
    if (value.empty() || value.length() % 2 != 0) {
        return false;
    }

    bytes.clear();
    for (std::size_t index = 0; index < value.length(); index += 2) {
        try {
            std::size_t used = 0;
            const unsigned long byte = std::stoul(value.substr(index, 2), &used, 16);
            if (used != 2) {
                return false;
            }
            bytes.push_back(static_cast<unsigned char>(byte));
        } catch (...) {
            return false;
        }
    }
    return true;
}

std::string pbkdf2(const std::vector<unsigned char>& salt, const std::string& password,
                   ULONGLONG iterations) {
    BCRYPT_ALG_HANDLE algorithm = nullptr;
    if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr,
                                    BCRYPT_ALG_HANDLE_HMAC_FLAG) != 0) {
        return "";
    }

    std::string passwordBuffer = password;
    std::vector<unsigned char> saltBuffer = salt;
    std::vector<unsigned char> key(DERIVED_KEY_BYTES);

    const NTSTATUS status = BCryptDeriveKeyPBKDF2(
        algorithm,
        reinterpret_cast<PUCHAR>(passwordBuffer.data()), static_cast<ULONG>(passwordBuffer.size()),
        saltBuffer.data(), static_cast<ULONG>(saltBuffer.size()),
        iterations, key.data(), DERIVED_KEY_BYTES, 0);

    BCryptCloseAlgorithmProvider(algorithm, 0);
    return status == 0 ? toHex(key) : "";
}

// Constant-time comparison so response timing does not reveal how many characters matched.
bool sameValue(const std::string& first, const std::string& second) {
    if (first.length() != second.length()) {
        return false;
    }

    unsigned char difference = 0;
    for (std::size_t index = 0; index < first.length(); ++index) {
        difference |= static_cast<unsigned char>(first[index] ^ second[index]);
    }
    return difference == 0;
}

std::vector<std::string> split(const std::string& value, char separator) {
    std::vector<std::string> parts;
    std::string current;
    for (char character : value) {
        if (character == separator) {
            parts.push_back(current);
            current.clear();
        } else {
            current += character;
        }
    }
    parts.push_back(current);
    return parts;
}
}

std::string PasswordHasher::hashPassword(const std::string& password) {
    std::vector<unsigned char> salt(SALT_BYTES);
    if (BCryptGenRandom(nullptr, salt.data(), static_cast<ULONG>(salt.size()),
                        BCRYPT_USE_SYSTEM_PREFERRED_RNG) != 0) {
        return "";
    }

    const std::string hash = pbkdf2(salt, password, PBKDF2_ITERATIONS);
    if (hash.empty()) {
        return "";
    }
    return "pbkdf2$" + std::to_string(PBKDF2_ITERATIONS) + "$" + toHex(salt) + "$" + hash;
}

bool PasswordHasher::verifyPassword(const std::string& password, const std::string& storedValue) {
    const std::vector<std::string> parts = split(storedValue, '$');
    if (parts.size() != 4 || parts[0] != "pbkdf2") {
        return false;
    }

    ULONGLONG iterations = 0;
    try {
        iterations = std::stoull(parts[1]);
    } catch (...) {
        return false;
    }
    if (iterations == 0 || iterations > 10000000) {
        return false;
    }

    std::vector<unsigned char> salt;
    if (!fromHex(parts[2], salt)) {
        return false;
    }

    const std::string computed = pbkdf2(salt, password, iterations);
    return !computed.empty() && sameValue(computed, parts[3]);
}

std::string PasswordHasher::createSessionToken() {
    std::vector<unsigned char> bytes(32);
    if (BCryptGenRandom(nullptr, bytes.data(), static_cast<ULONG>(bytes.size()),
                        BCRYPT_USE_SYSTEM_PREFERRED_RNG) != 0) {
        return "";
    }

    return toHex(bytes);
}
