#include "Database.h"

#include "../engine/BloodType.h"
#include "../engine/Donor.h"
#include "../engine/Geo.h"
#include "../engine/MatchRequest.h"

#include <iostream>

namespace {
// Used for donors who have never donated: far beyond the 90-day rule.
constexpr int NEVER_DONATED_DAYS = 100000;

// Reliability score = average of the starting 5.0 and every rating received:
//   (5.0 + sum of stars) / (1 + number of ratings)
// A new donor shows 5.0; each rating then moves the average. Written for an
// UPDATE on donor_profiles, so donor_profiles.user_id is the donor being scored.
const char* RELIABILITY_FORMULA_SQL =
    "(5.0 + COALESCE((SELECT SUM(stars) FROM ratings WHERE ratings.donor_id = donor_profiles.user_id), 0)) * 1.0 / "
    "(1 + (SELECT COUNT(*) FROM ratings WHERE ratings.donor_id = donor_profiles.user_id))";

// RAII wrapper so every prepared statement is always finalized.
class Statement {
public:
    Statement(sqlite3* connection, const char* sql) {
        prepared = sqlite3_prepare_v2(connection, sql, -1, &handle, nullptr) == SQLITE_OK;
    }
    ~Statement() {
        sqlite3_finalize(handle);
    }
    Statement(const Statement&) = delete;
    Statement& operator=(const Statement&) = delete;

    bool isPrepared() const {
        return prepared;
    }

    Statement& bind(int index, int value) {
        sqlite3_bind_int(handle, index, value);
        return *this;
    }
    Statement& bind(int index, double value) {
        sqlite3_bind_double(handle, index, value);
        return *this;
    }
    Statement& bind(int index, const std::string& value) {
        sqlite3_bind_text(handle, index, value.c_str(), -1, SQLITE_TRANSIENT);
        return *this;
    }
    Statement& bindNull(int index) {
        sqlite3_bind_null(handle, index);
        return *this;
    }

    int step() {
        return sqlite3_step(handle);
    }

    int columnInt(int column) const {
        return sqlite3_column_int(handle, column);
    }
    double columnDouble(int column) const {
        return sqlite3_column_double(handle, column);
    }
    // NULL-safe: returns an empty string instead of crashing on a NULL column.
    std::string columnText(int column) const {
        const unsigned char* text = sqlite3_column_text(handle, column);
        return text == nullptr ? std::string() : std::string(reinterpret_cast<const char*>(text));
    }

private:
    sqlite3_stmt* handle = nullptr;
    bool prepared = false;
};

const char* SCHEMA = R"(
    CREATE TABLE IF NOT EXISTS users (
        id INTEGER PRIMARY KEY AUTOINCREMENT,
        name TEXT NOT NULL CHECK(length(name) BETWEEN 1 AND 100),
        email TEXT NOT NULL UNIQUE COLLATE NOCASE,
        password_hash TEXT NOT NULL,
        security_question TEXT NOT NULL,
        security_answer_hash TEXT NOT NULL,
        blood_type TEXT NOT NULL CHECK(blood_type IN ('A+','A-','B+','B-','AB+','AB-','O+','O-')),
        area TEXT NOT NULL,
        latitude REAL NOT NULL CHECK(latitude BETWEEN -90 AND 90),
        longitude REAL NOT NULL CHECK(longitude BETWEEN -180 AND 180),
        notifications_seen_at TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP,
        created_at TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP
    );

    CREATE TABLE IF NOT EXISTS donor_profiles (
        user_id INTEGER PRIMARY KEY,
        phone TEXT NOT NULL,
        available INTEGER NOT NULL DEFAULT 1 CHECK(available IN (0, 1)),
        last_donation_date TEXT,
        reliability_score REAL NOT NULL DEFAULT 5.0 CHECK(reliability_score BETWEEN 0 AND 5),
        FOREIGN KEY(user_id) REFERENCES users(id) ON DELETE CASCADE
    );

    CREATE TABLE IF NOT EXISTS hospitals (
        id INTEGER PRIMARY KEY AUTOINCREMENT,
        name TEXT NOT NULL UNIQUE,
        address TEXT NOT NULL,
        latitude REAL NOT NULL CHECK(latitude BETWEEN -90 AND 90),
        longitude REAL NOT NULL CHECK(longitude BETWEEN -180 AND 180)
    );

    CREATE TABLE IF NOT EXISTS blood_requests (
        id INTEGER PRIMARY KEY AUTOINCREMENT,
        receiver_id INTEGER NOT NULL,
        hospital_id INTEGER NOT NULL,
        required_blood_type TEXT NOT NULL
            CHECK(required_blood_type IN ('A+','A-','B+','B-','AB+','AB-','O+','O-')),
        units_required INTEGER NOT NULL CHECK(units_required BETWEEN 1 AND 10),
        status TEXT NOT NULL DEFAULT 'open'
            CHECK(status IN ('open', 'fulfilled', 'completed', 'cancelled')),
        created_at TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP,
        updated_at TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP,
        FOREIGN KEY(receiver_id) REFERENCES users(id) ON DELETE CASCADE,
        FOREIGN KEY(hospital_id) REFERENCES hospitals(id)
    );

    -- One row each time a receiver invites a donor to a request.
    -- Health questionnaire answers are never stored, only when the donor passed it.
    CREATE TABLE IF NOT EXISTS invitations (
        id INTEGER PRIMARY KEY AUTOINCREMENT,
        request_id INTEGER NOT NULL,
        donor_id INTEGER NOT NULL,
        status TEXT NOT NULL DEFAULT 'pending'
            CHECK(status IN ('pending', 'accepted', 'rejected', 'cancelled', 'expired')),
        invited_at TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP,
        expires_at TEXT NOT NULL,
        responded_at TEXT,
        eligibility_confirmed_at TEXT,
        FOREIGN KEY(request_id) REFERENCES blood_requests(id) ON DELETE CASCADE,
        FOREIGN KEY(donor_id) REFERENCES users(id) ON DELETE CASCADE
    );

    -- A donor can have at most one live (pending or accepted) invitation per request.
    -- Expired or cancelled invitations do not block a new one.
    CREATE UNIQUE INDEX IF NOT EXISTS idx_invitations_live
        ON invitations(request_id, donor_id) WHERE status IN ('pending', 'accepted');

    CREATE TABLE IF NOT EXISTS ratings (
        id INTEGER PRIMARY KEY AUTOINCREMENT,
        request_id INTEGER NOT NULL,
        donor_id INTEGER NOT NULL,
        receiver_id INTEGER NOT NULL,
        stars INTEGER NOT NULL CHECK(stars BETWEEN 1 AND 5),
        created_at TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP,
        UNIQUE(request_id, donor_id),
        FOREIGN KEY(request_id) REFERENCES blood_requests(id) ON DELETE CASCADE,
        FOREIGN KEY(donor_id) REFERENCES users(id) ON DELETE CASCADE,
        FOREIGN KEY(receiver_id) REFERENCES users(id) ON DELETE CASCADE
    );

    CREATE TABLE IF NOT EXISTS sessions (
        token TEXT PRIMARY KEY,
        user_id INTEGER NOT NULL,
        created_at TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP,
        expires_at TEXT NOT NULL,
        FOREIGN KEY(user_id) REFERENCES users(id) ON DELETE CASCADE
    );

    CREATE TABLE IF NOT EXISTS login_attempts (
        email TEXT PRIMARY KEY COLLATE NOCASE,
        failed_count INTEGER NOT NULL DEFAULT 0,
        locked_until TEXT
    );

    CREATE INDEX IF NOT EXISTS idx_users_blood_type ON users(blood_type);
    CREATE INDEX IF NOT EXISTS idx_donor_profiles_available ON donor_profiles(available);
    CREATE INDEX IF NOT EXISTS idx_blood_requests_receiver ON blood_requests(receiver_id);
    CREATE INDEX IF NOT EXISTS idx_invitations_request ON invitations(request_id, status);
    CREATE INDEX IF NOT EXISTS idx_invitations_donor ON invitations(donor_id, status);
    CREATE INDEX IF NOT EXISTS idx_sessions_user ON sessions(user_id);
    CREATE INDEX IF NOT EXISTS idx_ratings_donor ON ratings(donor_id);
)";
}

Database::Database(const std::string& filePath) : connection(nullptr), filePath(filePath) {
}

Database::~Database() {
    close();
}

bool Database::open() {
    if (connection != nullptr) {
        return true;
    }

    const int flags = SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX;
    if (sqlite3_open_v2(filePath.c_str(), &connection, flags, nullptr) != SQLITE_OK) {
        fail("open database");
        close();
        return false;
    }

    sqlite3_busy_timeout(connection, 5000);
    return execute("PRAGMA foreign_keys = ON;");
}

bool Database::initialize() {
    int version = 0;
    {
        Statement statement(connection, "PRAGMA user_version;");
        if (!statement.isPrepared() || statement.step() != SQLITE_ROW) {
            return fail("read schema version");
        }
        version = statement.columnInt(0);
    }

    if ((version == 0 && tableExists("users")) || (version > 0 && version < SCHEMA_VERSION)) {
        setError("This database was created by an older version of LifeLink. "
                 "Delete " + filePath + " and start the server again.");
        return false;
    }
    if (version > SCHEMA_VERSION) {
        setError("This database was created by a newer version of LifeLink.");
        return false;
    }

    // Recalculate every donor's score on start-up, so older data follows the current rule.
    return execute(SCHEMA) &&
           execute("PRAGMA user_version = " + std::to_string(SCHEMA_VERSION) + ";") &&
           execute(std::string("UPDATE donor_profiles SET reliability_score = ") + RELIABILITY_FORMULA_SQL + ";") &&
           seedHospitals();
}

void Database::close() {
    if (connection != nullptr) {
        sqlite3_close(connection);
        connection = nullptr;
    }
}

bool Database::beginTransaction() {
    return execute("BEGIN IMMEDIATE;");
}

bool Database::commit() {
    return execute("COMMIT;");
}

void Database::rollback() {
    sqlite3_exec(connection, "ROLLBACK;", nullptr, nullptr, nullptr);
}

std::optional<int> Database::registerUser(const std::string& name, const std::string& email,
                                          const std::string& passwordHash,
                                          const std::string& securityQuestion,
                                          const std::string& securityAnswerHash,
                                          const std::string& bloodType, const std::string& area,
                                          double latitude, double longitude) {
    if (!isValidBloodType(bloodType)) {
        setError("Choose a valid blood group.");
        return std::nullopt;
    }
    Statement statement(connection,
        "INSERT INTO users (name, email, password_hash, security_question, security_answer_hash, "
        "blood_type, area, latitude, longitude) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?);");
    if (!statement.isPrepared()) {
        fail("prepare register user");
        return std::nullopt;
    }

    statement.bind(1, name).bind(2, email).bind(3, passwordHash).bind(4, securityQuestion)
             .bind(5, securityAnswerHash).bind(6, bloodType).bind(7, area).bind(8, latitude).bind(9, longitude);

    if (statement.step() != SQLITE_DONE) {
        if (sqlite3_extended_errcode(connection) == SQLITE_CONSTRAINT_UNIQUE) {
            setError("An account with this email already exists.");
        } else {
            fail("register user");
        }
        return std::nullopt;
    }

    return static_cast<int>(sqlite3_last_insert_rowid(connection));
}

bool Database::createDonorProfile(int userId, const std::string& phone,
                                  const std::optional<std::string>& lastDonationDate) {
    // Blood group and location come from the account; the donor profile adds
    // what only donating needs.
    Statement statement(connection,
        "INSERT INTO donor_profiles (user_id, phone, last_donation_date, reliability_score) VALUES (?, ?, ?, ?);");
    if (!statement.isPrepared()) {
        return fail("prepare donor profile");
    }

    // Set explicitly: a database created by an older version may still have 3.0 as the column default.
    statement.bind(1, userId).bind(2, phone).bind(4, INITIAL_RELIABILITY_SCORE);
    if (lastDonationDate.has_value()) {
        statement.bind(3, *lastDonationDate);
    } else {
        statement.bindNull(3);
    }

    return statement.step() == SQLITE_DONE || fail("create donor profile");
}

std::optional<UserAccount> Database::findUserByEmail(const std::string& email) const {
    Statement statement(connection, "SELECT id, name, email, EXISTS(SELECT 1 FROM donor_profiles WHERE user_id = users.id), blood_type, area, latitude, longitude FROM users WHERE email = ?;");
    if (!statement.isPrepared()) {
        fail("prepare find user by email");
        return std::nullopt;
    }

    statement.bind(1, email);
    if (statement.step() != SQLITE_ROW) {
        return std::nullopt;
    }

    return UserAccount{statement.columnInt(0), statement.columnText(1), statement.columnText(2),
                       statement.columnInt(3) != 0, statement.columnText(4), statement.columnText(5),
                       statement.columnDouble(6), statement.columnDouble(7)};
}

std::optional<UserAccount> Database::findUserById(int userId) const {
    Statement statement(connection, "SELECT id, name, email, EXISTS(SELECT 1 FROM donor_profiles WHERE user_id = users.id), blood_type, area, latitude, longitude FROM users WHERE id = ?;");
    if (!statement.isPrepared()) {
        fail("prepare find user by id");
        return std::nullopt;
    }

    statement.bind(1, userId);
    if (statement.step() != SQLITE_ROW) {
        return std::nullopt;
    }

    return UserAccount{statement.columnInt(0), statement.columnText(1), statement.columnText(2),
                       statement.columnInt(3) != 0, statement.columnText(4), statement.columnText(5),
                       statement.columnDouble(6), statement.columnDouble(7)};
}

nlohmann::json Database::getDonorProfile(int donorId) const {
    Statement statement(connection, R"(
        SELECT users.blood_type, donor_profiles.phone, donor_profiles.available,
               donor_profiles.last_donation_date, donor_profiles.reliability_score,
               COALESCE(CAST(julianday('now') - julianday(donor_profiles.last_donation_date) AS INTEGER), ?),
               (SELECT COUNT(*) FROM ratings WHERE ratings.donor_id = donor_profiles.user_id)
        FROM donor_profiles JOIN users ON users.id = donor_profiles.user_id
        WHERE donor_profiles.user_id = ?;
    )");
    if (!statement.isPrepared()) {
        fail("prepare donor profile");
        return nullptr;
    }
    statement.bind(1, NEVER_DONATED_DAYS).bind(2, donorId);
    if (statement.step() != SQLITE_ROW) {
        return nullptr;
    }
    const int daysSince = statement.columnInt(5);
    nlohmann::json profile = {
        {"blood_type", statement.columnText(0)},
        {"phone", statement.columnText(1)},
        {"available", statement.columnInt(2) != 0},
        {"reliability_score", statement.columnDouble(4)},
        {"ratings_count", statement.columnInt(6)},
        {"can_donate_again_in_days", daysSince >= MIN_DAYS_BETWEEN_DONATIONS ? 0 : MIN_DAYS_BETWEEN_DONATIONS - daysSince}
    };
    const std::string lastDonation = statement.columnText(3);
    profile["last_donation_date"] = lastDonation.empty() ? nlohmann::json(nullptr) : nlohmann::json(lastDonation);
    return profile;
}

bool Database::updateProfile(int userId, const std::string& name, const std::string& bloodType,
                             const std::string& area, double latitude, double longitude) {
    if (!isValidBloodType(bloodType)) {
        setError("Choose a valid blood group.");
        return false;
    }
    Statement statement(connection,
        "UPDATE users SET name = ?, blood_type = ?, area = ?, latitude = ?, longitude = ? WHERE id = ?;");
    if (!statement.isPrepared()) {
        return fail("prepare update profile");
    }
    statement.bind(1, name).bind(2, bloodType).bind(3, area).bind(4, latitude).bind(5, longitude).bind(6, userId);
    if (statement.step() != SQLITE_DONE) {
        return fail("update profile");
    }
    return true;
}

bool Database::updateDonorPhone(int donorId, const std::string& phone) {
    Statement statement(connection, "UPDATE donor_profiles SET phone = ? WHERE user_id = ?;");
    if (!statement.isPrepared()) {
        return fail("prepare update phone");
    }
    statement.bind(1, phone).bind(2, donorId);
    return statement.step() == SQLITE_DONE || fail("update phone");
}

bool Database::setDonorAvailability(int donorId, bool available) {
    Statement statement(connection, "UPDATE donor_profiles SET available = ? WHERE user_id = ?;");
    if (!statement.isPrepared()) {
        return fail("prepare set availability");
    }
    statement.bind(1, available ? 1 : 0).bind(2, donorId);
    if (statement.step() != SQLITE_DONE) {
        return fail("set availability");
    }
    if (sqlite3_changes(connection) != 1) {
        setError("Donor profile was not found.");
        return false;
    }
    return true;
}

std::optional<std::string> Database::getPasswordHash(const std::string& email) const {
    Statement statement(connection, "SELECT password_hash FROM users WHERE email = ?;");
    if (!statement.isPrepared()) {
        fail("prepare get password hash");
        return std::nullopt;
    }

    statement.bind(1, email);
    if (statement.step() != SQLITE_ROW) {
        return std::nullopt;
    }
    return statement.columnText(0);
}

std::optional<std::string> Database::getSecurityQuestion(const std::string& email) const {
    Statement statement(connection, "SELECT security_question FROM users WHERE email = ?;");
    if (!statement.isPrepared()) {
        fail("prepare get security question");
        return std::nullopt;
    }

    statement.bind(1, email);
    if (statement.step() != SQLITE_ROW) {
        return std::nullopt;
    }
    return statement.columnText(0);
}

std::optional<std::string> Database::getSecurityAnswerHash(const std::string& email) const {
    Statement statement(connection, "SELECT security_answer_hash FROM users WHERE email = ?;");
    if (!statement.isPrepared()) {
        fail("prepare get security answer");
        return std::nullopt;
    }

    statement.bind(1, email);
    if (statement.step() != SQLITE_ROW) {
        return std::nullopt;
    }
    return statement.columnText(0);
}

bool Database::updatePassword(const std::string& email, const std::string& passwordHash) {
    {
        Statement statement(connection, "UPDATE users SET password_hash = ? WHERE email = ?;");
        if (!statement.isPrepared()) {
            return fail("prepare update password");
        }

        statement.bind(1, passwordHash).bind(2, email);
        if (statement.step() != SQLITE_DONE) {
            return fail("update password");
        }
        if (sqlite3_changes(connection) != 1) {
            setError("No account found for this email.");
            return false;
        }
    }

    // A password reset logs the user out everywhere.
    Statement sessions(connection,
        "DELETE FROM sessions WHERE user_id = (SELECT id FROM users WHERE email = ?);");
    if (!sessions.isPrepared()) {
        return fail("prepare clear sessions");
    }
    sessions.bind(1, email);
    return sessions.step() == SQLITE_DONE || fail("clear sessions");
}

bool Database::createSession(int userId, const std::string& token) {
    Statement statement(connection,
        "INSERT INTO sessions (token, user_id, expires_at) VALUES (?, ?, datetime('now', ?));");
    if (!statement.isPrepared()) {
        return fail("prepare create session");
    }

    statement.bind(1, token).bind(2, userId)
             .bind(3, "+" + std::to_string(SESSION_LIFETIME_HOURS) + " hours");
    return statement.step() == SQLITE_DONE || fail("create session");
}

std::optional<int> Database::getSessionUserId(const std::string& token) const {
    Statement statement(connection,
        "SELECT user_id FROM sessions WHERE token = ? AND expires_at > datetime('now');");
    if (!statement.isPrepared()) {
        fail("prepare get session");
        return std::nullopt;
    }

    statement.bind(1, token);
    if (statement.step() != SQLITE_ROW) {
        return std::nullopt;
    }
    return statement.columnInt(0);
}

bool Database::deleteSession(const std::string& token) {
    Statement statement(connection, "DELETE FROM sessions WHERE token = ?;");
    if (!statement.isPrepared()) {
        return fail("prepare delete session");
    }
    statement.bind(1, token);
    return statement.step() == SQLITE_DONE || fail("delete session");
}

bool Database::deleteExpiredSessions() {
    return execute("DELETE FROM sessions WHERE expires_at <= datetime('now');");
}

bool Database::isLockedOut(const std::string& email) const {
    Statement statement(connection,
        "SELECT 1 FROM login_attempts WHERE email = ? AND locked_until > datetime('now');");
    if (!statement.isPrepared()) {
        fail("prepare lockout check");
        return false;
    }
    statement.bind(1, email);
    return statement.step() == SQLITE_ROW;
}

void Database::recordFailedAttempt(const std::string& email) {
    {
        Statement statement(connection,
            "INSERT INTO login_attempts (email, failed_count) VALUES (?, 1) "
            "ON CONFLICT(email) DO UPDATE SET failed_count = failed_count + 1;");
        if (!statement.isPrepared()) {
            fail("prepare record failed attempt");
            return;
        }
        statement.bind(1, email);
        if (statement.step() != SQLITE_DONE) {
            fail("record failed attempt");
            return;
        }
    }

    Statement lock(connection,
        "UPDATE login_attempts SET locked_until = datetime('now', ?), failed_count = 0 "
        "WHERE email = ? AND failed_count >= ?;");
    if (!lock.isPrepared()) {
        fail("prepare lock account");
        return;
    }
    lock.bind(1, "+" + std::to_string(LOCKOUT_MINUTES) + " minutes").bind(2, email)
        .bind(3, MAX_FAILED_ATTEMPTS);
    if (lock.step() != SQLITE_DONE) {
        fail("lock account");
    }
}

void Database::clearFailedAttempts(const std::string& email) {
    Statement statement(connection, "DELETE FROM login_attempts WHERE email = ?;");
    if (!statement.isPrepared()) {
        fail("prepare clear failed attempts");
        return;
    }
    statement.bind(1, email);
    if (statement.step() != SQLITE_DONE) {
        fail("clear failed attempts");
    }
}

nlohmann::json Database::getHospitals() const {
    nlohmann::json hospitals = nlohmann::json::array();
    Statement statement(connection, "SELECT id, name, address FROM hospitals ORDER BY name;");
    if (!statement.isPrepared()) {
        fail("prepare get hospitals");
        return hospitals;
    }

    while (statement.step() == SQLITE_ROW) {
        hospitals.push_back({
            {"id", statement.columnInt(0)},
            {"name", statement.columnText(1)},
            {"address", statement.columnText(2)}
        });
    }
    return hospitals;
}

std::optional<int> Database::createBloodRequest(int receiverId, int hospitalId,
                                                const std::string& bloodType, int unitsRequired) {
    if (!hospitalExists(hospitalId)) {
        setError("Choose a hospital from the available list.");
        return std::nullopt;
    }
    if (!isValidBloodType(bloodType)) {
        setError("Choose a valid blood group.");
        return std::nullopt;
    }
    if (unitsRequired < 1 || unitsRequired > MAX_UNITS_PER_REQUEST) {
        setError("Units required must be between 1 and " + std::to_string(MAX_UNITS_PER_REQUEST) + ".");
        return std::nullopt;
    }

    Statement statement(connection,
        "INSERT INTO blood_requests (receiver_id, hospital_id, required_blood_type, units_required) "
        "VALUES (?, ?, ?, ?);");
    if (!statement.isPrepared()) {
        fail("prepare create blood request");
        return std::nullopt;
    }

    statement.bind(1, receiverId).bind(2, hospitalId).bind(3, bloodType).bind(4, unitsRequired);
    if (statement.step() != SQLITE_DONE) {
        fail("create blood request");
        return std::nullopt;
    }
    return static_cast<int>(sqlite3_last_insert_rowid(connection));
}

std::optional<BloodRequestRecord> Database::getBloodRequest(int requestId) const {
    Statement statement(connection,
        "SELECT id, receiver_id, hospital_id, required_blood_type, units_required, status "
        "FROM blood_requests WHERE id = ?;");
    if (!statement.isPrepared()) {
        fail("prepare get blood request");
        return std::nullopt;
    }

    statement.bind(1, requestId);
    if (statement.step() != SQLITE_ROW) {
        return std::nullopt;
    }

    return BloodRequestRecord{statement.columnInt(0), statement.columnInt(1), statement.columnInt(2),
                              statement.columnText(3), statement.columnInt(4), statement.columnText(5)};
}

bool Database::updateRequestStatus(int requestId, const std::string& status) {
    Statement statement(connection,
        "UPDATE blood_requests SET status = ?, updated_at = CURRENT_TIMESTAMP WHERE id = ?;");
    if (!statement.isPrepared()) {
        return fail("prepare update request status");
    }
    statement.bind(1, status).bind(2, requestId);
    return statement.step() == SQLITE_DONE || fail("update request status");
}

int Database::loadCandidatesForRequest(MatchRequest& request, int receiverId) const {
    double hospitalLatitude = 0.0;
    double hospitalLongitude = 0.0;
    {
        Statement hospital(connection, "SELECT latitude, longitude FROM hospitals WHERE id = ?;");
        if (!hospital.isPrepared()) {
            fail("prepare load hospital");
            return 0;
        }
        hospital.bind(1, request.getHospitalId());
        if (hospital.step() != SQLITE_ROW) {
            setError("Hospital was not found.");
            return 0;
        }
        hospitalLatitude = hospital.columnDouble(0);
        hospitalLongitude = hospital.columnDouble(1);
    }

    // Days since last donation are computed from the stored date, so they never go stale.
    Statement donors(connection, R"(
        SELECT users.id, users.name, donor_profiles.phone, users.blood_type,
               users.latitude, users.longitude,
               COALESCE(CAST(julianday('now') - julianday(donor_profiles.last_donation_date) AS INTEGER), ?),
               donor_profiles.reliability_score, donor_profiles.available
        FROM users
        JOIN donor_profiles ON donor_profiles.user_id = users.id
        WHERE users.id != ? AND donor_profiles.available = 1
          -- not already invited (and still waiting), accepted, or rejected for this request
          AND NOT EXISTS (
              SELECT 1 FROM invitations
              WHERE invitations.request_id = ? AND invitations.donor_id = users.id
                AND invitations.status IN ('pending', 'accepted', 'rejected')
          )
          -- not already committed to another request that is still in progress
          AND NOT EXISTS (
              SELECT 1 FROM invitations
              JOIN blood_requests ON blood_requests.id = invitations.request_id
              WHERE invitations.donor_id = users.id AND invitations.status = 'accepted'
                AND blood_requests.status IN ('open', 'fulfilled')
          );
    )");
    if (!donors.isPrepared()) {
        fail("prepare load candidates");
        return 0;
    }

    donors.bind(1, NEVER_DONATED_DAYS).bind(2, receiverId).bind(3, request.getRequestId());

    int candidateCount = 0;
    while (donors.step() == SQLITE_ROW) {
        const double distanceKm = haversineKm(hospitalLatitude, hospitalLongitude,
                                              donors.columnDouble(4), donors.columnDouble(5));

        const Donor donor(donors.columnInt(0), donors.columnText(1), donors.columnText(2),
                          donors.columnText(3), distanceKm, donors.columnInt(6),
                          donors.columnDouble(7), donors.columnInt(8) != 0);

        // The engine checks eligibility, compatibility and the search radius.
        if (request.addCandidate(donor)) {
            ++candidateCount;
        }
    }
    return candidateCount;
}

bool Database::completeRequest(int requestId) {
    {
        // Accepting is a promise; the donation date is recorded only once the receiver
        // confirms the donation happened. This restarts the donor's 90-day interval.
        Statement donation(connection, R"(
            UPDATE donor_profiles SET last_donation_date = date('now')
            WHERE user_id IN (SELECT donor_id FROM invitations
                              WHERE request_id = ? AND status = 'accepted');
        )");
        if (!donation.isPrepared()) {
            return fail("prepare record donation");
        }
        donation.bind(1, requestId);
        if (donation.step() != SQLITE_DONE) {
            return fail("record donation");
        }
    }
    return updateRequestStatus(requestId, RequestStatus::COMPLETED);
}

bool Database::expireOldInvitations() {
    return execute("UPDATE invitations SET status = 'expired' "
                   "WHERE status = 'pending' AND expires_at <= datetime('now');");
}

std::optional<int> Database::createInvitation(int requestId, int donorId) {
    Statement statement(connection,
        "INSERT INTO invitations (request_id, donor_id, expires_at) VALUES (?, ?, datetime('now', ?));");
    if (!statement.isPrepared()) {
        fail("prepare create invitation");
        return std::nullopt;
    }
    statement.bind(1, requestId).bind(2, donorId)
             .bind(3, "+" + std::to_string(INVITATION_LIFETIME_MINUTES) + " minutes");
    if (statement.step() != SQLITE_DONE) {
        if (sqlite3_extended_errcode(connection) == SQLITE_CONSTRAINT_UNIQUE) {
            setError("This donor has already been invited to this request.");
        } else {
            fail("create invitation");
        }
        return std::nullopt;
    }
    return static_cast<int>(sqlite3_last_insert_rowid(connection));
}

std::optional<InvitationRecord> Database::getInvitation(int invitationId) const {
    Statement statement(connection,
        "SELECT id, request_id, donor_id, status FROM invitations WHERE id = ?;");
    if (!statement.isPrepared()) {
        fail("prepare get invitation");
        return std::nullopt;
    }
    statement.bind(1, invitationId);
    if (statement.step() != SQLITE_ROW) {
        return std::nullopt;
    }
    return InvitationRecord{statement.columnInt(0), statement.columnInt(1), statement.columnInt(2),
                            statement.columnText(3)};
}

int Database::countInvitations(int requestId, const std::string& status) const {
    Statement statement(connection,
        "SELECT COUNT(*) FROM invitations WHERE request_id = ? AND status = ?;");
    if (!statement.isPrepared()) {
        fail("prepare count invitations");
        return 0;
    }
    statement.bind(1, requestId).bind(2, status);
    return statement.step() == SQLITE_ROW ? statement.columnInt(0) : 0;
}

bool Database::respondToInvitation(int invitationId, bool accepted) {
    Statement statement(connection, accepted
        ? "UPDATE invitations SET status = 'accepted', responded_at = CURRENT_TIMESTAMP, "
          "eligibility_confirmed_at = CURRENT_TIMESTAMP WHERE id = ? AND status = 'pending';"
        : "UPDATE invitations SET status = 'rejected', responded_at = CURRENT_TIMESTAMP "
          "WHERE id = ? AND status = 'pending';");
    if (!statement.isPrepared()) {
        return fail("prepare respond to invitation");
    }
    statement.bind(1, invitationId);
    if (statement.step() != SQLITE_DONE) {
        return fail("respond to invitation");
    }
    if (sqlite3_changes(connection) != 1) {
        setError("This invitation is no longer waiting for a response.");
        return false;
    }
    return true;
}

int Database::cancelPendingInvitations(int requestId) {
    Statement statement(connection,
        "UPDATE invitations SET status = 'cancelled', responded_at = CURRENT_TIMESTAMP "
        "WHERE request_id = ? AND status = 'pending';");
    if (!statement.isPrepared()) {
        fail("prepare cancel invitations");
        return -1;
    }
    statement.bind(1, requestId);
    if (statement.step() != SQLITE_DONE) {
        fail("cancel invitations");
        return -1;
    }
    return sqlite3_changes(connection);
}

bool Database::hasActiveCommitment(int donorId) const {
    Statement statement(connection, R"(
        SELECT 1 FROM invitations
        JOIN blood_requests ON blood_requests.id = invitations.request_id
        WHERE invitations.donor_id = ? AND invitations.status = 'accepted'
          AND blood_requests.status IN ('open', 'fulfilled');
    )");
    if (!statement.isPrepared()) {
        fail("prepare commitment check");
        return true;  // fail safe: do not allow a second acceptance on error
    }
    statement.bind(1, donorId);
    return statement.step() == SQLITE_ROW;
}

int Database::cancelDonorPendingInvitations(int donorId) {
    Statement statement(connection,
        "UPDATE invitations SET status = 'cancelled', responded_at = CURRENT_TIMESTAMP "
        "WHERE donor_id = ? AND status = 'pending';");
    if (!statement.isPrepared()) {
        fail("prepare cancel donor invitations");
        return -1;
    }
    statement.bind(1, donorId);
    if (statement.step() != SQLITE_DONE) {
        fail("cancel donor invitations");
        return -1;
    }
    return sqlite3_changes(connection);
}

nlohmann::json Database::getNotifications(int userId) const {
    nlohmann::json items = nlohmann::json::array();
    int unread = 0;

    {
        // As a donor: every invitation still waiting for a reply.
        Statement statement(connection, R"(
            SELECT invitations.id, invitations.invited_at, invitations.expires_at, blood_requests.required_blood_type,
                   hospitals.name, users.name
            FROM invitations
            JOIN blood_requests ON blood_requests.id = invitations.request_id
            JOIN hospitals ON hospitals.id = blood_requests.hospital_id
            JOIN users ON users.id = blood_requests.receiver_id
            WHERE invitations.donor_id = ? AND invitations.status = 'pending'
            ORDER BY invitations.invited_at DESC, invitations.id DESC;
        )");
        if (!statement.isPrepared()) {
            fail("prepare donor notifications");
            return {{"items", items}, {"unread", 0}};
        }
        statement.bind(1, userId);
        while (statement.step() == SQLITE_ROW) {
            ++unread;
            items.push_back({
                {"type", "invitation"},
                {"invitation_id", statement.columnInt(0)},
                {"at", statement.columnText(1)},
                {"expires_at", statement.columnText(2)},
                {"blood_type", statement.columnText(3)},
                {"hospital_name", statement.columnText(4)},
                {"person", statement.columnText(5)},
                {"unread", true}
            });
        }
    }

    {
        // As a receiver: donors who accepted in the last 7 days. New ones are unread.
        Statement statement(connection, R"(
            SELECT invitations.request_id, invitations.responded_at, donors.name, hospitals.name,
                   blood_requests.required_blood_type,
                   invitations.responded_at > (SELECT notifications_seen_at FROM users WHERE id = ?)
            FROM invitations
            JOIN blood_requests ON blood_requests.id = invitations.request_id
            JOIN hospitals ON hospitals.id = blood_requests.hospital_id
            JOIN users AS donors ON donors.id = invitations.donor_id
            WHERE blood_requests.receiver_id = ? AND invitations.status = 'accepted'
              AND invitations.responded_at >= datetime('now', '-7 days')
            ORDER BY invitations.responded_at DESC;
        )");
        if (!statement.isPrepared()) {
            fail("prepare receiver notifications");
            return {{"items", items}, {"unread", unread}};
        }
        statement.bind(1, userId).bind(2, userId);
        while (statement.step() == SQLITE_ROW) {
            const bool isUnread = statement.columnInt(5) != 0;
            if (isUnread) {
                ++unread;
            }
            items.push_back({
                {"type", "accepted"},
                {"request_id", statement.columnInt(0)},
                {"at", statement.columnText(1)},
                {"person", statement.columnText(2)},
                {"hospital_name", statement.columnText(3)},
                {"blood_type", statement.columnText(4)},
                {"unread", isUnread}
            });
        }
    }

    return {{"items", items}, {"unread", unread}};
}

bool Database::markNotificationsSeen(int userId) {
    Statement statement(connection, "UPDATE users SET notifications_seen_at = CURRENT_TIMESTAMP WHERE id = ?;");
    if (!statement.isPrepared()) {
        return fail("prepare mark notifications");
    }
    statement.bind(1, userId);
    return statement.step() == SQLITE_DONE || fail("mark notifications");
}

nlohmann::json Database::getDonorInvitations(int donorId) const {
    nlohmann::json invitations = nlohmann::json::array();
    // Everything from the last 30 days, newest first. The receiver's details are only
    // what the donor needs: who is asking and which hospital to go to.
    Statement statement(connection, R"(
        SELECT invitations.id, invitations.status, invitations.invited_at, invitations.expires_at,
               invitations.responded_at, blood_requests.id, blood_requests.status,
               blood_requests.required_blood_type, blood_requests.units_required,
               hospitals.name, hospitals.address, users.name
        FROM invitations
        JOIN blood_requests ON blood_requests.id = invitations.request_id
        JOIN hospitals ON hospitals.id = blood_requests.hospital_id
        JOIN users ON users.id = blood_requests.receiver_id
        WHERE invitations.donor_id = ? AND invitations.invited_at >= datetime('now', '-30 days')
        ORDER BY invitations.invited_at DESC, invitations.id DESC;
    )");
    if (!statement.isPrepared()) {
        fail("prepare donor invitations");
        return invitations;
    }
    statement.bind(1, donorId);
    while (statement.step() == SQLITE_ROW) {
        invitations.push_back({
            {"invitation_id", statement.columnInt(0)},
            {"status", statement.columnText(1)},
            {"invited_at", statement.columnText(2)},
            {"expires_at", statement.columnText(3)},
            {"responded_at", statement.columnText(4)},
            {"request_id", statement.columnInt(5)},
            {"request_status", statement.columnText(6)},
            {"blood_type", statement.columnText(7)},
            {"units_required", statement.columnInt(8)},
            {"hospital_name", statement.columnText(9)},
            {"hospital_address", statement.columnText(10)},
            {"receiver_name", statement.columnText(11)}
        });
    }
    return invitations;
}

nlohmann::json Database::getRequestInvitations(int requestId) const {
    nlohmann::json invitations = nlohmann::json::array();
    Statement statement(connection, R"(
        SELECT invitations.id, invitations.status, invitations.invited_at, invitations.expires_at,
               invitations.responded_at, users.id, users.name, users.blood_type,
               donor_profiles.phone, donor_profiles.reliability_score,
               EXISTS(SELECT 1 FROM ratings WHERE ratings.request_id = invitations.request_id
                                             AND ratings.donor_id = users.id)
        FROM invitations
        JOIN users ON users.id = invitations.donor_id
        JOIN donor_profiles ON donor_profiles.user_id = users.id
        WHERE invitations.request_id = ?
        ORDER BY invitations.invited_at, invitations.id;
    )");
    if (!statement.isPrepared()) {
        fail("prepare request invitations");
        return invitations;
    }
    statement.bind(1, requestId);
    while (statement.step() == SQLITE_ROW) {
        const std::string status = statement.columnText(1);
        nlohmann::json invitation = {
            {"invitation_id", statement.columnInt(0)},
            {"status", status},
            {"invited_at", statement.columnText(2)},
            {"expires_at", statement.columnText(3)},
            {"responded_at", statement.columnText(4)},
            {"donor_id", statement.columnInt(5)},
            {"donor_name", statement.columnText(6)},
            {"blood_type", statement.columnText(7)},
            {"reliability_score", statement.columnDouble(9)},
            {"rated", statement.columnInt(10) != 0}
        };
        // The donor's phone number is shared only after they have accepted.
        if (status == InvitationStatus::ACCEPTED) {
            invitation["donor_phone"] = statement.columnText(8);
        }
        invitations.push_back(invitation);
    }
    return invitations;
}

nlohmann::json Database::getReceiverRequests(int receiverId, std::optional<int> onlyRequestId) const {
    nlohmann::json requests = nlohmann::json::array();
    Statement statement(connection, R"(
        SELECT blood_requests.id, blood_requests.status, blood_requests.required_blood_type,
               blood_requests.units_required, blood_requests.created_at,
               hospitals.id, hospitals.name, hospitals.address
        FROM blood_requests
        JOIN hospitals ON hospitals.id = blood_requests.hospital_id
        WHERE blood_requests.receiver_id = ? AND (? IS NULL OR blood_requests.id = ?)
        ORDER BY blood_requests.created_at DESC, blood_requests.id DESC;
    )");
    if (!statement.isPrepared()) {
        fail("prepare receiver requests");
        return requests;
    }
    statement.bind(1, receiverId);
    if (onlyRequestId.has_value()) {
        statement.bind(2, *onlyRequestId).bind(3, *onlyRequestId);
    } else {
        statement.bindNull(2).bindNull(3);
    }

    while (statement.step() == SQLITE_ROW) {
        const int requestId = statement.columnInt(0);
        requests.push_back({
            {"request_id", requestId},
            {"status", statement.columnText(1)},
            {"blood_type", statement.columnText(2)},
            {"units_required", statement.columnInt(3)},
            {"created_at", statement.columnText(4)},
            {"hospital_id", statement.columnInt(5)},
            {"hospital_name", statement.columnText(6)},
            {"hospital_address", statement.columnText(7)},
            {"units_accepted", countInvitations(requestId, InvitationStatus::ACCEPTED)},
            {"open_invitations", countInvitations(requestId, InvitationStatus::PENDING)},
            {"invitations", getRequestInvitations(requestId)}
        });
    }
    return requests;
}

bool Database::addRating(int requestId, int receiverId, int donorId, int stars) {
    if (stars < 1 || stars > 5) {
        setError("Rating must be between 1 and 5 stars.");
        return false;
    }

    {
        // Only the receiver who made the request may rate, only after the donation
        // is completed, and only a donor who accepted it.
        Statement check(connection, R"(
            SELECT 1 FROM blood_requests
            JOIN invitations ON invitations.request_id = blood_requests.id
            WHERE blood_requests.id = ? AND blood_requests.receiver_id = ?
              AND blood_requests.status = 'completed'
              AND invitations.donor_id = ? AND invitations.status = 'accepted';
        )");
        if (!check.isPrepared()) {
            return fail("prepare rating check");
        }
        check.bind(1, requestId).bind(2, receiverId).bind(3, donorId);
        if (check.step() != SQLITE_ROW) {
            setError("You can rate a donor only after they accepted your request and the donation is completed.");
            return false;
        }
    }

    {
        Statement insert(connection,
            "INSERT INTO ratings (request_id, donor_id, receiver_id, stars) VALUES (?, ?, ?, ?);");
        if (!insert.isPrepared()) {
            return fail("prepare add rating");
        }
        insert.bind(1, requestId).bind(2, donorId).bind(3, receiverId).bind(4, stars);
        if (insert.step() != SQLITE_DONE) {
            if (sqlite3_extended_errcode(connection) == SQLITE_CONSTRAINT_UNIQUE) {
                setError("You have already rated this donor for this request.");
                return false;
            }
            return fail("add rating");
        }
    }

    const std::string updateSql =
        std::string("UPDATE donor_profiles SET reliability_score = ") + RELIABILITY_FORMULA_SQL + " WHERE user_id = ?;";
    Statement update(connection, updateSql.c_str());
    if (!update.isPrepared()) {
        return fail("prepare update reliability");
    }
    update.bind(1, donorId);
    return update.step() == SQLITE_DONE || fail("update reliability");
}

bool Database::updateDonorReliability(int donorId, double reliabilityScore) {
    if (reliabilityScore < 0.0 || reliabilityScore > 5.0) {
        setError("Reliability score must be between 0 and 5.");
        return false;
    }

    Statement statement(connection, "UPDATE donor_profiles SET reliability_score = ? WHERE user_id = ?;");
    if (!statement.isPrepared()) {
        return fail("prepare update reliability");
    }
    statement.bind(1, reliabilityScore).bind(2, donorId);
    if (statement.step() != SQLITE_DONE) {
        return fail("update reliability");
    }
    if (sqlite3_changes(connection) != 1) {
        setError("Donor was not found.");
        return false;
    }
    return true;
}

const std::string& Database::getLastError() const {
    return lastError;
}

bool Database::execute(const std::string& sql) {
    char* errorMessage = nullptr;
    if (sqlite3_exec(connection, sql.c_str(), nullptr, nullptr, &errorMessage) != SQLITE_OK) {
        std::cerr << "[database] " << (errorMessage == nullptr ? "unknown error" : errorMessage) << '\n';
        sqlite3_free(errorMessage);
        lastError = "A database error occurred. Please try again.";
        return false;
    }
    return true;
}

bool Database::seedHospitals() {
    return execute(R"(
        INSERT OR IGNORE INTO hospitals (name, address, latitude, longitude)
        VALUES ('Doon Hospital', 'Near Parade Ground, Dehradun', 30.3236, 78.0410),
               ('Coronation Hospital', 'Chandar Nagar, Dehradun', 30.3200, 78.0280),
               ('Shri Mahant Indiresh Hospital', 'Patel Nagar, Dehradun', 30.3060, 78.0220),
               ('Synergy Institute of Medical Sciences', 'Ballupur, Dehradun', 30.3335, 78.0110),
               ('Max Super Speciality Hospital', 'Malsi, Mussoorie Road, Dehradun', 30.3730, 78.0740),
               ('CMI Hospital', 'Dalanwala, Dehradun', 30.3150, 78.0520);
    )");
}

bool Database::hospitalExists(int hospitalId) const {
    Statement statement(connection, "SELECT 1 FROM hospitals WHERE id = ?;");
    if (!statement.isPrepared()) {
        fail("prepare hospital check");
        return false;
    }
    statement.bind(1, hospitalId);
    return statement.step() == SQLITE_ROW;
}

bool Database::tableExists(const std::string& tableName) const {
    Statement statement(connection, "SELECT 1 FROM sqlite_master WHERE type = 'table' AND name = ?;");
    if (!statement.isPrepared()) {
        return false;
    }
    statement.bind(1, tableName);
    return statement.step() == SQLITE_ROW;
}

void Database::setError(const std::string& message) const {
    lastError = message;
}

bool Database::fail(const char* context) const {
    std::cerr << "[database] " << context << ": "
              << (connection == nullptr ? "no connection" : sqlite3_errmsg(connection)) << '\n';
    lastError = "A database error occurred. Please try again.";
    return false;
}
