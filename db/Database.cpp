#include "Database.h"

#include "../engine/BloodType.h"
#include "../engine/Donor.h"
#include "../engine/MatchRequest.h"

#include <cmath>

namespace {
bool prepareStatement(sqlite3* connection, const char* sql, sqlite3_stmt** statement,
                      std::string& error) {
    if (sqlite3_prepare_v2(connection, sql, -1, statement, nullptr) != SQLITE_OK) {
        error = sqlite3_errmsg(connection);
        return false;
    }

    return true;
}
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

    if (sqlite3_open(filePath.c_str(), &connection) != SQLITE_OK) {
        setError(sqlite3_errmsg(connection));
        close();
        return false;
    }

    return execute("PRAGMA foreign_keys = ON;");
}

bool Database::initialize() {
    const std::string schema = R"(
        CREATE TABLE IF NOT EXISTS users (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            name TEXT NOT NULL,
            email TEXT NOT NULL UNIQUE,
            password_hash TEXT NOT NULL,
            role TEXT NOT NULL CHECK(role IN ('donor', 'receiver')),
            security_question TEXT NOT NULL,
            security_answer TEXT NOT NULL,
            created_at TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP
        );

        CREATE TABLE IF NOT EXISTS donor_profiles (
            user_id INTEGER PRIMARY KEY,
            phone TEXT NOT NULL,
            blood_type TEXT NOT NULL,
            latitude REAL NOT NULL,
            longitude REAL NOT NULL,
            available INTEGER NOT NULL DEFAULT 1,
            days_since_last_donation INTEGER NOT NULL DEFAULT 365,
            reliability_score REAL NOT NULL DEFAULT 3.0,
            FOREIGN KEY(user_id) REFERENCES users(id)
        );

        CREATE TABLE IF NOT EXISTS hospitals (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            name TEXT NOT NULL,
            address TEXT NOT NULL,
            latitude REAL NOT NULL,
            longitude REAL NOT NULL
        );

        CREATE TABLE IF NOT EXISTS blood_requests (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            receiver_id INTEGER NOT NULL,
            hospital_id INTEGER NOT NULL,
            required_blood_type TEXT NOT NULL,
            units_required INTEGER NOT NULL CHECK(units_required > 0),
            status TEXT NOT NULL DEFAULT 'pending',
            created_at TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP,
            FOREIGN KEY(receiver_id) REFERENCES users(id),
            FOREIGN KEY(hospital_id) REFERENCES hospitals(id)
        );

        CREATE TABLE IF NOT EXISTS donor_responses (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            request_id INTEGER NOT NULL,
            donor_id INTEGER NOT NULL,
            accepted INTEGER NOT NULL,
            responded_at TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP,
            FOREIGN KEY(request_id) REFERENCES blood_requests(id),
            FOREIGN KEY(donor_id) REFERENCES users(id)
        );

        CREATE TABLE IF NOT EXISTS sessions (
            token TEXT PRIMARY KEY,
            user_id INTEGER NOT NULL,
            created_at TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP,
            FOREIGN KEY(user_id) REFERENCES users(id)
        );
    )";

    return execute(schema) && seedHospitals();
}

void Database::close() {
    if (connection != nullptr) {
        sqlite3_close(connection);
        connection = nullptr;
    }
}

std::optional<int> Database::registerUser(const std::string& name, const std::string& email,
                                          const std::string& passwordHash, const std::string& role,
                                          const std::string& securityQuestion,
                                          const std::string& securityAnswer) {
    if (role != "donor" && role != "receiver") {
        setError("Role must be donor or receiver.");
        return std::nullopt;
    }

    const char* sql = "INSERT INTO users (name, email, password_hash, role, security_question, security_answer) "
                      "VALUES (?, ?, ?, ?, ?, ?);";
    sqlite3_stmt* statement = nullptr;
    if (!prepareStatement(connection, sql, &statement, lastError)) {
        return std::nullopt;
    }

    sqlite3_bind_text(statement, 1, name.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(statement, 2, email.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(statement, 3, passwordHash.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(statement, 4, role.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(statement, 5, securityQuestion.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(statement, 6, securityAnswer.c_str(), -1, SQLITE_TRANSIENT);

    if (sqlite3_step(statement) != SQLITE_DONE) {
        setError(sqlite3_errmsg(connection));
        sqlite3_finalize(statement);
        return std::nullopt;
    }

    sqlite3_finalize(statement);
    return static_cast<int>(sqlite3_last_insert_rowid(connection));
}

bool Database::createDonorProfile(int userId, const std::string& phone, const std::string& bloodType,
                                  double latitude, double longitude) {
    if (!isValidBloodType(bloodType)) {
        setError("Invalid blood type.");
        return false;
    }

    const char* sql = "INSERT INTO donor_profiles (user_id, phone, blood_type, latitude, longitude) "
                      "VALUES (?, ?, ?, ?, ?);";
    sqlite3_stmt* statement = nullptr;
    if (!prepareStatement(connection, sql, &statement, lastError)) {
        return false;
    }

    sqlite3_bind_int(statement, 1, userId);
    sqlite3_bind_text(statement, 2, phone.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(statement, 3, bloodType.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_double(statement, 4, latitude);
    sqlite3_bind_double(statement, 5, longitude);

    const bool created = sqlite3_step(statement) == SQLITE_DONE;
    if (!created) {
        setError(sqlite3_errmsg(connection));
    }
    sqlite3_finalize(statement);
    return created;
}

std::optional<UserAccount> Database::findUserByEmail(const std::string& email) const {
    const char* sql = "SELECT id, name, email, role FROM users WHERE email = ?;";
    sqlite3_stmt* statement = nullptr;
    if (!prepareStatement(connection, sql, &statement, lastError)) {
        return std::nullopt;
    }

    sqlite3_bind_text(statement, 1, email.c_str(), -1, SQLITE_TRANSIENT);

    if (sqlite3_step(statement) != SQLITE_ROW) {
        sqlite3_finalize(statement);
        return std::nullopt;
    }

    UserAccount user{
        sqlite3_column_int(statement, 0),
        reinterpret_cast<const char*>(sqlite3_column_text(statement, 1)),
        reinterpret_cast<const char*>(sqlite3_column_text(statement, 2)),
        reinterpret_cast<const char*>(sqlite3_column_text(statement, 3))
    };
    sqlite3_finalize(statement);
    return user;
}

std::optional<UserAccount> Database::findUserById(int userId) const {
    const char* sql = "SELECT id, name, email, role FROM users WHERE id = ?;";
    sqlite3_stmt* statement = nullptr;
    if (!prepareStatement(connection, sql, &statement, lastError)) {
        return std::nullopt;
    }

    sqlite3_bind_int(statement, 1, userId);
    if (sqlite3_step(statement) != SQLITE_ROW) {
        sqlite3_finalize(statement);
        return std::nullopt;
    }

    UserAccount user{
        sqlite3_column_int(statement, 0),
        reinterpret_cast<const char*>(sqlite3_column_text(statement, 1)),
        reinterpret_cast<const char*>(sqlite3_column_text(statement, 2)),
        reinterpret_cast<const char*>(sqlite3_column_text(statement, 3))
    };
    sqlite3_finalize(statement);
    return user;
}

std::optional<std::string> Database::getPasswordHash(const std::string& email) const {
    const char* sql = "SELECT password_hash FROM users WHERE email = ?;";
    sqlite3_stmt* statement = nullptr;
    if (!prepareStatement(connection, sql, &statement, lastError)) {
        return std::nullopt;
    }

    sqlite3_bind_text(statement, 1, email.c_str(), -1, SQLITE_TRANSIENT);
    if (sqlite3_step(statement) != SQLITE_ROW) {
        sqlite3_finalize(statement);
        return std::nullopt;
    }

    const std::string passwordHash = reinterpret_cast<const char*>(sqlite3_column_text(statement, 0));
    sqlite3_finalize(statement);
    return passwordHash;
}

bool Database::verifySecurityAnswer(const std::string& email, const std::string& answer) const {
    const char* sql = "SELECT 1 FROM users WHERE email = ? AND security_answer = ?;";
    sqlite3_stmt* statement = nullptr;
    if (!prepareStatement(connection, sql, &statement, lastError)) {
        return false;
    }

    sqlite3_bind_text(statement, 1, email.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(statement, 2, answer.c_str(), -1, SQLITE_TRANSIENT);
    const bool correct = sqlite3_step(statement) == SQLITE_ROW;
    sqlite3_finalize(statement);
    return correct;
}

bool Database::updatePassword(const std::string& email, const std::string& passwordHash) {
    const char* sql = "UPDATE users SET password_hash = ? WHERE email = ?;";
    sqlite3_stmt* statement = nullptr;
    if (!prepareStatement(connection, sql, &statement, lastError)) {
        return false;
    }

    sqlite3_bind_text(statement, 1, passwordHash.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(statement, 2, email.c_str(), -1, SQLITE_TRANSIENT);
    const bool updated = sqlite3_step(statement) == SQLITE_DONE && sqlite3_changes(connection) == 1;
    if (!updated) {
        setError(sqlite3_errmsg(connection));
    }
    sqlite3_finalize(statement);
    return updated;
}

bool Database::createSession(int userId, const std::string& token) {
    const char* sql = "INSERT INTO sessions (token, user_id) VALUES (?, ?);";
    sqlite3_stmt* statement = nullptr;
    if (!prepareStatement(connection, sql, &statement, lastError)) {
        return false;
    }

    sqlite3_bind_text(statement, 1, token.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(statement, 2, userId);
    const bool created = sqlite3_step(statement) == SQLITE_DONE;
    if (!created) {
        setError(sqlite3_errmsg(connection));
    }
    sqlite3_finalize(statement);
    return created;
}

std::optional<int> Database::getSessionUserId(const std::string& token) const {
    const char* sql = "SELECT user_id FROM sessions WHERE token = ?;";
    sqlite3_stmt* statement = nullptr;
    if (!prepareStatement(connection, sql, &statement, lastError)) {
        return std::nullopt;
    }

    sqlite3_bind_text(statement, 1, token.c_str(), -1, SQLITE_TRANSIENT);
    if (sqlite3_step(statement) != SQLITE_ROW) {
        sqlite3_finalize(statement);
        return std::nullopt;
    }

    const int userId = sqlite3_column_int(statement, 0);
    sqlite3_finalize(statement);
    return userId;
}

nlohmann::json Database::getHospitals() const {
    nlohmann::json hospitals = nlohmann::json::array();
    const char* sql = "SELECT id, name, address FROM hospitals ORDER BY name;";
    sqlite3_stmt* statement = nullptr;
    if (!prepareStatement(connection, sql, &statement, lastError)) {
        return hospitals;
    }

    while (sqlite3_step(statement) == SQLITE_ROW) {
        hospitals.push_back({
            {"id", sqlite3_column_int(statement, 0)},
            {"name", reinterpret_cast<const char*>(sqlite3_column_text(statement, 1))},
            {"address", reinterpret_cast<const char*>(sqlite3_column_text(statement, 2))}
        });
    }

    sqlite3_finalize(statement);
    return hospitals;
}

std::optional<int> Database::createBloodRequest(int receiverId, int hospitalId,
                                                 const std::string& bloodType, int unitsRequired) {
    if (!hospitalExists(hospitalId)) {
        setError("Choose a hospital from the available list.");
        return std::nullopt;
    }
    if (!isValidBloodType(bloodType) || unitsRequired < 1) {
        setError("Blood type or units requested are invalid.");
        return std::nullopt;
    }

    const char* sql = "INSERT INTO blood_requests (receiver_id, hospital_id, required_blood_type, units_required) "
                      "VALUES (?, ?, ?, ?);";
    sqlite3_stmt* statement = nullptr;
    if (!prepareStatement(connection, sql, &statement, lastError)) {
        return std::nullopt;
    }

    sqlite3_bind_int(statement, 1, receiverId);
    sqlite3_bind_int(statement, 2, hospitalId);
    sqlite3_bind_text(statement, 3, bloodType.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(statement, 4, unitsRequired);
    if (sqlite3_step(statement) != SQLITE_DONE) {
        setError(sqlite3_errmsg(connection));
        sqlite3_finalize(statement);
        return std::nullopt;
    }

    sqlite3_finalize(statement);
    return static_cast<int>(sqlite3_last_insert_rowid(connection));
}

std::optional<BloodRequestRecord> Database::getBloodRequest(int requestId) const {
    const char* sql = "SELECT id, receiver_id, hospital_id, required_blood_type, units_required "
                      "FROM blood_requests WHERE id = ?;";
    sqlite3_stmt* statement = nullptr;
    if (!prepareStatement(connection, sql, &statement, lastError)) {
        return std::nullopt;
    }

    sqlite3_bind_int(statement, 1, requestId);
    if (sqlite3_step(statement) != SQLITE_ROW) {
        sqlite3_finalize(statement);
        return std::nullopt;
    }

    BloodRequestRecord request{
        sqlite3_column_int(statement, 0),
        sqlite3_column_int(statement, 1),
        sqlite3_column_int(statement, 2),
        reinterpret_cast<const char*>(sqlite3_column_text(statement, 3)),
        sqlite3_column_int(statement, 4)
    };
    sqlite3_finalize(statement);
    return request;
}

int Database::loadCandidatesForRequest(MatchRequest& request) const {
    const char* hospitalSql = "SELECT latitude, longitude FROM hospitals WHERE id = ?;";
    sqlite3_stmt* hospitalStatement = nullptr;
    if (!prepareStatement(connection, hospitalSql, &hospitalStatement, lastError)) {
        return 0;
    }

    sqlite3_bind_int(hospitalStatement, 1, request.getHospitalId());
    if (sqlite3_step(hospitalStatement) != SQLITE_ROW) {
        setError("Hospital was not found.");
        sqlite3_finalize(hospitalStatement);
        return 0;
    }

    const double hospitalLatitude = sqlite3_column_double(hospitalStatement, 0);
    const double hospitalLongitude = sqlite3_column_double(hospitalStatement, 1);
    sqlite3_finalize(hospitalStatement);

    const char* donorSql = R"(
        SELECT users.id, users.name, donor_profiles.phone, donor_profiles.blood_type,
               donor_profiles.latitude, donor_profiles.longitude,
               donor_profiles.days_since_last_donation, donor_profiles.reliability_score,
               donor_profiles.available
        FROM users
        JOIN donor_profiles ON donor_profiles.user_id = users.id
        WHERE users.role = 'donor' AND donor_profiles.available = 1
          AND NOT EXISTS (
              SELECT 1 FROM donor_responses
              WHERE donor_responses.request_id = ? AND donor_responses.donor_id = users.id
          );
    )";
    sqlite3_stmt* donorStatement = nullptr;
    if (!prepareStatement(connection, donorSql, &donorStatement, lastError)) {
        return 0;
    }

    sqlite3_bind_int(donorStatement, 1, request.getRequestId());

    int candidateCount = 0;
    while (sqlite3_step(donorStatement) == SQLITE_ROW) {
        const double latitude = sqlite3_column_double(donorStatement, 4);
        const double longitude = sqlite3_column_double(donorStatement, 5);
        const double latitudeDistance = (latitude - hospitalLatitude) * 111.0;
        const double longitudeDistance = (longitude - hospitalLongitude) * 111.0;
        const double distanceKm = std::sqrt(latitudeDistance * latitudeDistance +
                                            longitudeDistance * longitudeDistance);

        Donor donor(
            sqlite3_column_int(donorStatement, 0),
            reinterpret_cast<const char*>(sqlite3_column_text(donorStatement, 1)),
            reinterpret_cast<const char*>(sqlite3_column_text(donorStatement, 2)),
            reinterpret_cast<const char*>(sqlite3_column_text(donorStatement, 3)),
            distanceKm,
            sqlite3_column_int(donorStatement, 6),
            sqlite3_column_double(donorStatement, 7),
            sqlite3_column_int(donorStatement, 8) != 0
        );

        if (request.addCandidate(donor)) {
            ++candidateCount;
        }
    }

    sqlite3_finalize(donorStatement);
    return candidateCount;
}

nlohmann::json Database::getNearbyLowerRatedDonors(const MatchRequest& request) const {
    nlohmann::json donors = nlohmann::json::array();
    const char* sql = R"(
        SELECT users.id, users.name, donor_profiles.phone, donor_profiles.blood_type,
               donor_profiles.latitude, donor_profiles.longitude,
               donor_profiles.days_since_last_donation, donor_profiles.reliability_score,
               donor_profiles.available, hospitals.latitude, hospitals.longitude
        FROM users
        JOIN donor_profiles ON donor_profiles.user_id = users.id
        JOIN hospitals ON hospitals.id = ?
        WHERE users.role = 'donor' AND donor_profiles.available = 1
          AND NOT EXISTS (
              SELECT 1 FROM donor_responses
              WHERE donor_responses.request_id = ? AND donor_responses.donor_id = users.id
          );
    )";
    sqlite3_stmt* statement = nullptr;
    if (!prepareStatement(connection, sql, &statement, lastError)) {
        return donors;
    }

    sqlite3_bind_int(statement, 1, request.getHospitalId());
    sqlite3_bind_int(statement, 2, request.getRequestId());

    while (sqlite3_step(statement) == SQLITE_ROW) {
        const double latitudeDistance = (sqlite3_column_double(statement, 4) -
                                         sqlite3_column_double(statement, 9)) * 111.0;
        const double longitudeDistance = (sqlite3_column_double(statement, 5) -
                                          sqlite3_column_double(statement, 10)) * 111.0;
        const double distanceKm = std::sqrt(latitudeDistance * latitudeDistance +
                                            longitudeDistance * longitudeDistance);
        const double reliabilityScore = sqlite3_column_double(statement, 7);
        Donor donor(
            sqlite3_column_int(statement, 0),
            reinterpret_cast<const char*>(sqlite3_column_text(statement, 1)),
            reinterpret_cast<const char*>(sqlite3_column_text(statement, 2)),
            reinterpret_cast<const char*>(sqlite3_column_text(statement, 3)),
            distanceKm,
            sqlite3_column_int(statement, 6),
            reliabilityScore,
            sqlite3_column_int(statement, 8) != 0
        );

        if (donor.isEligible() && canDonateTo(donor.getBloodType(), request.getRequiredBloodType()) &&
            donor.getDistanceKm() <= NEARBY_DISTANCE_THRESHOLD_KM &&
            donor.getReliabilityScore() < LOW_RELIABILITY_THRESHOLD) {
            donors.push_back({
                {"id", donor.getId()},
                {"name", donor.getName()},
                {"phone", donor.getPhone()},
                {"blood_type", donor.getBloodType()},
                {"distance_km", donor.getDistanceKm()},
                {"reliability_score", donor.getReliabilityScore()}
            });
        }
    }

    sqlite3_finalize(statement);
    return donors;
}

bool Database::saveDonorResponse(int requestId, int donorId, bool accepted) {
    const char* responseSql = "INSERT INTO donor_responses (request_id, donor_id, accepted) VALUES (?, ?, ?);";
    sqlite3_stmt* responseStatement = nullptr;
    if (!prepareStatement(connection, responseSql, &responseStatement, lastError)) {
        return false;
    }

    sqlite3_bind_int(responseStatement, 1, requestId);
    sqlite3_bind_int(responseStatement, 2, donorId);
    sqlite3_bind_int(responseStatement, 3, accepted ? 1 : 0);
    const bool saved = sqlite3_step(responseStatement) == SQLITE_DONE;
    if (!saved) {
        setError(sqlite3_errmsg(connection));
    }
    sqlite3_finalize(responseStatement);

    if (!saved || !accepted) {
        return saved;
    }

    const char* requestSql = "UPDATE blood_requests SET status = 'matched' WHERE id = ?;";
    sqlite3_stmt* requestStatement = nullptr;
    if (!prepareStatement(connection, requestSql, &requestStatement, lastError)) {
        return false;
    }

    sqlite3_bind_int(requestStatement, 1, requestId);
    const bool updated = sqlite3_step(requestStatement) == SQLITE_DONE;
    if (!updated) {
        setError(sqlite3_errmsg(connection));
    }
    sqlite3_finalize(requestStatement);
    return updated;
}

bool Database::updateDonorReliability(int donorId, double reliabilityScore) {
    if (reliabilityScore < 0.0 || reliabilityScore > 5.0) {
        setError("Reliability score must be between 0 and 5.");
        return false;
    }

    const char* sql = "UPDATE donor_profiles SET reliability_score = ? WHERE user_id = ?;";
    sqlite3_stmt* statement = nullptr;
    if (!prepareStatement(connection, sql, &statement, lastError)) {
        return false;
    }

    sqlite3_bind_double(statement, 1, reliabilityScore);
    sqlite3_bind_int(statement, 2, donorId);
    const bool updated = sqlite3_step(statement) == SQLITE_DONE && sqlite3_changes(connection) == 1;
    if (!updated) {
        setError(sqlite3_errmsg(connection));
    }
    sqlite3_finalize(statement);
    return updated;
}

const std::string& Database::getLastError() const {
    return lastError;
}

bool Database::execute(const std::string& sql) {
    char* errorMessage = nullptr;
    if (sqlite3_exec(connection, sql.c_str(), nullptr, nullptr, &errorMessage) != SQLITE_OK) {
        setError(errorMessage == nullptr ? "Database operation failed." : errorMessage);
        sqlite3_free(errorMessage);
        return false;
    }

    return true;
}

bool Database::seedHospitals() {
    return execute(R"(
        INSERT INTO hospitals (name, address, latitude, longitude)
        SELECT 'LifeLink Central Hospital', 'Central Bengaluru', 12.9716, 77.5946
        WHERE NOT EXISTS (SELECT 1 FROM hospitals WHERE name = 'LifeLink Central Hospital');

        INSERT INTO hospitals (name, address, latitude, longitude)
        SELECT 'LifeLink East Hospital', 'Indiranagar, Bengaluru', 12.9784, 77.6408
        WHERE NOT EXISTS (SELECT 1 FROM hospitals WHERE name = 'LifeLink East Hospital');

        INSERT INTO hospitals (name, address, latitude, longitude)
        SELECT 'LifeLink South Hospital', 'Jayanagar, Bengaluru', 12.9250, 77.5938
        WHERE NOT EXISTS (SELECT 1 FROM hospitals WHERE name = 'LifeLink South Hospital');
    )");
}

bool Database::hospitalExists(int hospitalId) const {
    const char* sql = "SELECT 1 FROM hospitals WHERE id = ?;";
    sqlite3_stmt* statement = nullptr;
    if (!prepareStatement(connection, sql, &statement, lastError)) {
        return false;
    }

    sqlite3_bind_int(statement, 1, hospitalId);
    const bool exists = sqlite3_step(statement) == SQLITE_ROW;
    sqlite3_finalize(statement);
    return exists;
}

void Database::setError(const std::string& message) const {
    lastError = message;
}
