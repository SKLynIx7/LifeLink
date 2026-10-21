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
    bool isDonor;  // true once the user has set up a donor profile
    std::string bloodType;
    std::string area;
    double latitude;
    double longitude;
};

struct BloodRequestRecord {
    int id;
    int receiverId;
    int hospitalId;
    std::string bloodType;
    int unitsRequired;
    std::string status;
};

struct InvitationRecord {
    int id;
    int requestId;
    int donorId;
    std::string status;
};

// Request lifecycle: open -> fulfilled (enough donors accepted) -> completed (donation done).
// The receiver can cancel an open or fulfilled request.
namespace RequestStatus {
inline const std::string OPEN = "open";
inline const std::string FULFILLED = "fulfilled";
inline const std::string COMPLETED = "completed";
inline const std::string CANCELLED = "cancelled";
}

// Invitation lifecycle: pending -> accepted / rejected (donor's choice),
// cancelled (another donor accepted first, or the receiver cancelled),
// expired (no answer within INVITATION_LIFETIME_MINUTES).
namespace InvitationStatus {
inline const std::string PENDING = "pending";
inline const std::string ACCEPTED = "accepted";
inline const std::string REJECTED = "rejected";
inline const std::string CANCELLED = "cancelled";
inline const std::string EXPIRED = "expired";
}

constexpr int SCHEMA_VERSION = 5;
constexpr int SESSION_LIFETIME_HOURS = 24;
constexpr int MAX_FAILED_ATTEMPTS = 5;
constexpr int LOCKOUT_MINUTES = 15;
constexpr int MAX_UNITS_PER_REQUEST = 10;
constexpr int INVITATION_LIFETIME_MINUTES = 60;
// Every donor starts with this rating, and it counts as one rating in their average.
constexpr double INITIAL_RELIABILITY_SCORE = 5.0;
constexpr int MAX_OPEN_INVITATIONS_PER_REQUEST = 10;

class Database {
public:
    explicit Database(const std::string& filePath);
    ~Database();

    Database(const Database&) = delete;
    Database& operator=(const Database&) = delete;

    bool open();
    bool initialize();
    void close();

    // Transactions (prefer the Transaction helper below).
    bool beginTransaction();
    bool commit();
    void rollback();

    // ---- Accounts. Emails must already be normalised (trimmed + lowercase).
    // One account can both request blood and donate; donating needs a donor profile.
    std::optional<int> registerUser(const std::string& name, const std::string& email,
                                    const std::string& passwordHash,
                                    const std::string& securityQuestion,
                                    const std::string& securityAnswerHash,
                                    const std::string& bloodType, const std::string& area,
                                    double latitude, double longitude);
    bool createDonorProfile(int userId, const std::string& phone,
                            const std::optional<std::string>& lastDonationDate = std::nullopt);
    std::optional<UserAccount> findUserByEmail(const std::string& email) const;
    std::optional<UserAccount> findUserById(int userId) const;
    nlohmann::json getDonorProfile(int donorId) const;
    bool setDonorAvailability(int donorId, bool available);
    // Profile page: details the user can change after registering.
    bool updateProfile(int userId, const std::string& name, const std::string& bloodType,
                       const std::string& area, double latitude, double longitude);
    bool updateDonorPhone(int donorId, const std::string& phone);
    std::optional<std::string> getPasswordHash(const std::string& email) const;
    std::optional<std::string> getSecurityQuestion(const std::string& email) const;
    std::optional<std::string> getSecurityAnswerHash(const std::string& email) const;
    // Also ends every existing session of that user.
    bool updatePassword(const std::string& email, const std::string& passwordHash);

    // ---- Sessions expire after SESSION_LIFETIME_HOURS.
    bool createSession(int userId, const std::string& token);
    std::optional<int> getSessionUserId(const std::string& token) const;
    bool deleteSession(const std::string& token);
    bool deleteExpiredSessions();

    // ---- Brute-force protection for login and password reset.
    bool isLockedOut(const std::string& email) const;
    void recordFailedAttempt(const std::string& email);
    void clearFailedAttempts(const std::string& email);

    // ---- Hospitals and requests.
    nlohmann::json getHospitals() const;
    std::optional<int> createBloodRequest(int receiverId, int hospitalId,
                                          const std::string& bloodType, int unitsRequired);
    std::optional<BloodRequestRecord> getBloodRequest(int requestId) const;
    bool updateRequestStatus(int requestId, const std::string& status);
    // Marks the request completed and records today's donation date for the accepted donors.
    bool completeRequest(int requestId);

    // ---- Matching. Loads eligible donors who can still be invited to this request:
    // not the receiver themself, not already pending, accepted or rejected for it,
    // and not committed to another open request.
    int loadCandidatesForRequest(MatchRequest& request, int receiverId) const;

    // ---- Invitations.
    // Marks every pending invitation older than INVITATION_LIFETIME_MINUTES as expired.
    bool expireOldInvitations();
    std::optional<int> createInvitation(int requestId, int donorId);
    std::optional<InvitationRecord> getInvitation(int invitationId) const;
    int countInvitations(int requestId, const std::string& status) const;
    // Accepting records when the donor passed the eligibility questionnaire (not the answers).
    bool respondToInvitation(int invitationId, bool accepted);
    // Cancels all still-pending invitations of a request. Returns how many were cancelled.
    int cancelPendingInvitations(int requestId);
    // True when the donor has accepted a request that is not yet completed or cancelled.
    bool hasActiveCommitment(int donorId) const;
    // Cancels the donor's waiting invitations for other requests once they commit to one.
    int cancelDonorPendingInvitations(int donorId);

    // ---- Notifications (the bell). Donors: invitations waiting for a reply.
    // Receivers: donors who accepted since the receiver last opened the bell.
    nlohmann::json getNotifications(int userId) const;
    bool markNotificationsSeen(int userId);

    // ---- Dashboards.
    nlohmann::json getDonorInvitations(int donorId) const;
    nlohmann::json getReceiverRequests(int receiverId, std::optional<int> onlyRequestId = std::nullopt) const;

    // ---- Ratings: reliability_score = average of the starting 5.0 and every rating received.
    bool addRating(int requestId, int receiverId, int donorId, int stars);
    bool updateDonorReliability(int donorId, double reliabilityScore);

    // A message that is safe to show to end users.
    const std::string& getLastError() const;

private:
    bool execute(const std::string& sql);
    bool seedHospitals();
    bool hospitalExists(int hospitalId) const;
    bool tableExists(const std::string& tableName) const;
    nlohmann::json getRequestInvitations(int requestId) const;
    void setError(const std::string& message) const;
    // Logs the real SQLite error to stderr and stores a generic message for the user.
    bool fail(const char* context) const;

    sqlite3* connection;
    std::string filePath;
    mutable std::string lastError;
};

// Rolls back automatically unless commit() succeeds.
class Transaction {
public:
    explicit Transaction(Database& database) : database(database), active(database.beginTransaction()) {
    }
    ~Transaction() {
        if (active) {
            database.rollback();
        }
    }
    Transaction(const Transaction&) = delete;
    Transaction& operator=(const Transaction&) = delete;

    bool ok() const {
        return active;
    }

    bool commit() {
        if (!active) {
            return false;
        }
        active = false;
        if (!database.commit()) {
            database.rollback();
            return false;
        }
        return true;
    }

private:
    Database& database;
    bool active;
};

#endif
