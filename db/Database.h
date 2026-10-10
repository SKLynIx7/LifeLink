#ifndef DATABASE_H
#define DATABASE_H

#include <nlohmann/json.hpp>
#include <sqlite3.h>

#include <optional>
#include <string>

class MatchRequest;

struct UserAccount {
    int id;
    std::string name;
    std::string email;
    std::string role;
};

struct BloodRequestRecord {
    int id;
    int receiverId;
    int hospitalId;
    std::string bloodType;
    int unitsRequired;
};

class Database {
public:
    explicit Database(const std::string& filePath);
    ~Database();

    Database(const Database&) = delete;
    Database& operator=(const Database&) = delete;

    bool open();
    bool initialize();
    void close();

    std::optional<int> registerUser(const std::string& name, const std::string& email,
                                    const std::string& passwordHash, const std::string& role,
                                    const std::string& securityQuestion,
                                    const std::string& securityAnswer);
    bool createDonorProfile(int userId, const std::string& phone, const std::string& bloodType,
                            double latitude, double longitude);
    std::optional<UserAccount> findUserByEmail(const std::string& email) const;
    std::optional<UserAccount> findUserById(int userId) const;
    std::optional<std::string> getPasswordHash(const std::string& email) const;
    bool verifySecurityAnswer(const std::string& email, const std::string& answer) const;
    bool updatePassword(const std::string& email, const std::string& passwordHash);
    bool createSession(int userId, const std::string& token);
    std::optional<int> getSessionUserId(const std::string& token) const;

    nlohmann::json getHospitals() const;
    std::optional<int> createBloodRequest(int receiverId, int hospitalId,
                                          const std::string& bloodType, int unitsRequired);
    std::optional<BloodRequestRecord> getBloodRequest(int requestId) const;
    int loadCandidatesForRequest(MatchRequest& request) const;
    nlohmann::json getNearbyLowerRatedDonors(const MatchRequest& request) const;
    bool saveDonorResponse(int requestId, int donorId, bool accepted);
    bool updateDonorReliability(int donorId, double reliabilityScore);

    const std::string& getLastError() const;

private:
    bool execute(const std::string& sql);
    bool seedHospitals();
    bool hospitalExists(int hospitalId) const;
    void setError(const std::string& message) const;

    sqlite3* connection;
    std::string filePath;
    mutable std::string lastError;
};

#endif
