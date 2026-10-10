#include "PasswordHasher.h"

#include <windows.h>
#include <bcrypt.h>

#include <iomanip>
#include <sstream>
#include <vector>

namespace {
std::string toHex(const std::vector<unsigned char>& bytes) {
    std::ostringstream output;
    for (unsigned char byte : bytes) {
        output << std::hex << std::setw(2) << std::setfill('0') << static_cast<int>(byte);
    }
    return output.str();
}

bool fromHex(const std::string& value, std::vector<unsigned char>& bytes) {
    if (value.length() % 2 != 0) {
        return false;
    }

    bytes.clear();
    for (std::size_t index = 0; index < value.length(); index += 2) {
        try {
            bytes.push_back(static_cast<unsigned char>(std::stoul(value.substr(index, 2), nullptr, 16)));
        } catch (...) {
            return false;
        }
    }
    return true;
}

std::string sha256(const std::vector<unsigned char>& salt, const std::string& password) {
    BCRYPT_ALG_HANDLE algorithm = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    DWORD hashSize = 0;
    DWORD resultSize = 0;

    if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) != 0 ||
        BCryptGetProperty(algorithm, BCRYPT_HASH_LENGTH,
                          reinterpret_cast<PUCHAR>(&hashSize), sizeof(hashSize),
                          &resultSize, 0) != 0) {
        if (algorithm != nullptr) {
            BCryptCloseAlgorithmProvider(algorithm, 0);
        }
        return "";
    }

    std::vector<unsigned char> hashValue(hashSize);
    std::string input(reinterpret_cast<const char*>(salt.data()), salt.size());
    input += password;

    const NTSTATUS created = BCryptCreateHash(algorithm, &hash, nullptr, 0, nullptr, 0, 0);
    const NTSTATUS hashed = created == 0
        ? BCryptHashData(hash, reinterpret_cast<PUCHAR>(input.data()), static_cast<ULONG>(input.size()), 0)
        : created;
    const NTSTATUS finished = hashed == 0
        ? BCryptFinishHash(hash, hashValue.data(), hashSize, 0)
        : hashed;

    if (hash != nullptr) {
        BCryptDestroyHash(hash);
    }
    BCryptCloseAlgorithmProvider(algorithm, 0);

    return finished == 0 ? toHex(hashValue) : "";
}

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
}

std::string PasswordHasher::hashPassword(const std::string& password) {
    std::vector<unsigned char> salt(16);
    if (BCryptGenRandom(nullptr, salt.data(), static_cast<ULONG>(salt.size()),
                        BCRYPT_USE_SYSTEM_PREFERRED_RNG) != 0) {
        return "";
    }

    const std::string passwordHash = sha256(salt, password);
    return passwordHash.empty() ? "" : toHex(salt) + ":" + passwordHash;
}

bool PasswordHasher::verifyPassword(const std::string& password, const std::string& storedValue) {
    const std::size_t separator = storedValue.find(':');
    if (separator == std::string::npos) {
        return false;
    }

    std::vector<unsigned char> salt;
    if (!fromHex(storedValue.substr(0, separator), salt)) {
        return false;
    }

    return sameValue(sha256(salt, password), storedValue.substr(separator + 1));
}

std::string PasswordHasher::createSessionToken() {
    std::vector<unsigned char> bytes(32);
    if (BCryptGenRandom(nullptr, bytes.data(), static_cast<ULONG>(bytes.size()),
                        BCRYPT_USE_SYSTEM_PREFERRED_RNG) != 0) {
        return "";
    }

    return toHex(bytes);
}
