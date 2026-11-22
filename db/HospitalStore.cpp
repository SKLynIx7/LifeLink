// Hospital accounts: registration, login sessions, profile, location and availability.
// Part of the Database class (declared in Database.h); split out to keep Database.cpp readable.

#include "Database.h"

#include "../engine/Geo.h"
#include "Statement.h"

#include <algorithm>
#include <sstream>

using db::Statement;

namespace {
constexpr int HOSPITAL_SESSION_LIFETIME_HOURS = SESSION_LIFETIME_HOURS;

std::string joinList(const std::vector<std::string>& items) {
    std::string joined;
    for (const std::string& item : items) {
        if (!joined.empty()) {
            joined += ',';
        }
        joined += item;
    }
    return joined;
}

std::vector<std::string> splitList(const std::string& text) {
    std::vector<std::string> items;
    std::stringstream stream(text);
    std::string item;
    while (std::getline(stream, item, ',')) {
        if (!item.empty()) {
            items.push_back(item);
        }
    }
    return items;
}

// Every query that reads a whole hospital selects these columns in this order.
const char* HOSPITAL_COLUMNS =
    "id, name, COALESCE(email, ''), phone, emergency_phone, address, city, state, country, pincode, "
    "place_id, latitude, longitude, hospital_type, license_number, available_blood_groups, opening_time, "
    "closing_time, working_days, website, description, facilities, availability_status, "
    "CASE WHEN closes_at IS NULL THEN NULL "
    "     ELSE CAST(ROUND((julianday(closes_at) - julianday('now')) * 86400) AS INTEGER) END, "
    "closure_reason, verification_status, verification_note, COALESCE(registered_at, '')";

HospitalRecord readHospital(const Statement& row) {
    HospitalRecord hospital;
    hospital.id = row.columnInt(0);
    hospital.name = row.columnText(1);
    hospital.email = row.columnText(2);
    hospital.phone = row.columnText(3);
    hospital.emergencyPhone = row.columnText(4);
    hospital.address = row.columnText(5);
    hospital.city = row.columnText(6);
    hospital.state = row.columnText(7);
    hospital.country = row.columnText(8);
    hospital.pincode = row.columnText(9);
    hospital.placeId = row.columnText(10);
    hospital.latitude = row.columnDouble(11);
    hospital.longitude = row.columnDouble(12);
    hospital.hospitalType = row.columnText(13);
    hospital.licenseNumber = row.columnText(14);
    hospital.bloodGroups = splitList(row.columnText(15));
    hospital.openingTime = row.columnText(16);
    hospital.closingTime = row.columnText(17);
    hospital.workingDays = splitList(row.columnText(18));
    hospital.website = row.columnText(19);
    hospital.description = row.columnText(20);
    hospital.facilities = splitList(row.columnText(21));
    hospital.availabilityState = row.columnText(22);
    if (!row.columnIsNull(23)) {
        hospital.secondsUntilManualClose = row.columnInt64(23);
    }
    hospital.closureReason = row.columnText(24);
    hospital.verificationStatus = row.columnText(25);
    hospital.verificationNote = row.columnText(26);
    hospital.registeredAt = row.columnText(27);
    return hospital;
}
}  // namespace

std::optional<int> Database::registerHospital(const HospitalRecord& hospital, const std::string& passwordHash) {
    Statement statement(connection, R"(
        INSERT INTO hospitals (name, email, password_hash, phone, emergency_phone, address, city, state,
                               country, pincode, place_id, latitude, longitude, hospital_type, license_number,
                               available_blood_groups, opening_time, closing_time, working_days, website,
                               description, facilities, availability_status, status_updated_at, registered_at,
                               verification_status)
        VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, 'open',
                CURRENT_TIMESTAMP, CURRENT_TIMESTAMP, 'pending');
    )");
    if (!statement.isPrepared()) {
        fail("prepare register hospital");
        return std::nullopt;
    }
    statement.bind(1, hospital.name).bind(2, hospital.email).bind(3, passwordHash).bind(4, hospital.phone)
             .bind(5, hospital.emergencyPhone).bind(6, hospital.address).bind(7, hospital.city)
             .bind(8, hospital.state).bind(9, hospital.country).bind(10, hospital.pincode)
             .bind(11, hospital.placeId).bind(12, hospital.latitude).bind(13, hospital.longitude)
             .bind(14, hospital.hospitalType).bind(15, hospital.licenseNumber)
             .bind(16, joinList(hospital.bloodGroups)).bind(17, hospital.openingTime)
             .bind(18, hospital.closingTime).bind(19, joinList(hospital.workingDays))
             .bind(20, hospital.website).bind(21, hospital.description).bind(22, joinList(hospital.facilities));
    if (statement.step() != SQLITE_DONE) {
        if (sqlite3_extended_errcode(connection) == SQLITE_CONSTRAINT_UNIQUE) {
            const std::string detail = sqlite3_errmsg(connection);
            setError(detail.find("email") != std::string::npos
                         ? "A hospital with this email is already registered."
                         : "A hospital with this name is already registered.");
        } else {
            fail("register hospital");
        }
        return std::nullopt;
    }
    return static_cast<int>(sqlite3_last_insert_rowid(connection));
}

std::optional<HospitalRecord> Database::getHospital(int hospitalId) const {
    const std::string sql = std::string("SELECT ") + HOSPITAL_COLUMNS + " FROM hospitals WHERE id = ?;";
    Statement statement(connection, sql.c_str());
    if (!statement.isPrepared()) {
        fail("prepare get hospital");
        return std::nullopt;
    }
    statement.bind(1, hospitalId);
    if (statement.step() != SQLITE_ROW) {
        return std::nullopt;
    }
    return readHospital(statement);
}

std::vector<HospitalRecord> Database::listHospitals() const {
    std::vector<HospitalRecord> hospitals;
    const std::string sql = std::string("SELECT ") + HOSPITAL_COLUMNS + " FROM hospitals ORDER BY name;";
    Statement statement(connection, sql.c_str());
    if (!statement.isPrepared()) {
        fail("prepare list hospitals");
        return hospitals;
    }
    while (statement.step() == SQLITE_ROW) {
        hospitals.push_back(readHospital(statement));
    }
    return hospitals;
}

std::vector<HospitalRecord> Database::listHospitalsNear(double latitude, double longitude, double radiusKm) const {
    std::vector<HospitalRecord> hospitals;
    // Same geospatial approach as the donor search: an indexed box, then the exact distance.
    const BoundingBox box = boundingBoxAround(latitude, longitude, radiusKm);
    const std::string sql = std::string("SELECT ") + HOSPITAL_COLUMNS +
        " FROM hospitals WHERE latitude BETWEEN ? AND ? AND longitude BETWEEN ? AND ?;";
    Statement statement(connection, sql.c_str());
    if (!statement.isPrepared()) {
        fail("prepare hospitals near");
        return hospitals;
    }
    statement.bind(1, box.minLatitude).bind(2, box.maxLatitude).bind(3, box.minLongitude).bind(4, box.maxLongitude);
    while (statement.step() == SQLITE_ROW) {
        HospitalRecord hospital = readHospital(statement);
        hospital.distanceKm = haversineKm(latitude, longitude, hospital.latitude, hospital.longitude);
        if (hospital.distanceKm <= radiusKm) {
            hospitals.push_back(std::move(hospital));
        }
    }
    return hospitals;
}

std::optional<int> Database::findHospitalIdByEmail(const std::string& email) const {
    Statement statement(connection, "SELECT id FROM hospitals WHERE email = ?;");
    if (!statement.isPrepared()) {
        fail("prepare find hospital");
        return std::nullopt;
    }
    statement.bind(1, email);
    if (statement.step() != SQLITE_ROW) {
        return std::nullopt;
    }
    return statement.columnInt(0);
}

std::optional<std::string> Database::getHospitalPasswordHash(const std::string& email) const {
    Statement statement(connection, "SELECT password_hash FROM hospitals WHERE email = ? AND password_hash IS NOT NULL;");
    if (!statement.isPrepared()) {
        fail("prepare hospital password");
        return std::nullopt;
    }
    statement.bind(1, email);
    if (statement.step() != SQLITE_ROW) {
        return std::nullopt;
    }
    return statement.columnText(0);
}

bool Database::createHospitalSession(int hospitalId, const std::string& token) {
    Statement statement(connection,
        "INSERT INTO hospital_sessions (token, hospital_id, expires_at) VALUES (?, ?, datetime('now', ?));");
    if (!statement.isPrepared()) {
        return fail("prepare hospital session");
    }
    statement.bind(1, token).bind(2, hospitalId)
             .bind(3, "+" + std::to_string(HOSPITAL_SESSION_LIFETIME_HOURS) + " hours");
    return statement.step() == SQLITE_DONE || fail("create hospital session");
}

std::optional<int> Database::getHospitalSessionId(const std::string& token) const {
    Statement statement(connection,
        "SELECT hospital_id FROM hospital_sessions WHERE token = ? AND expires_at > datetime('now');");
    if (!statement.isPrepared()) {
        fail("prepare get hospital session");
        return std::nullopt;
    }
    statement.bind(1, token);
    if (statement.step() != SQLITE_ROW) {
        return std::nullopt;
    }
    return statement.columnInt(0);
}

bool Database::deleteHospitalSession(const std::string& token) {
    Statement statement(connection, "DELETE FROM hospital_sessions WHERE token = ?;");
    if (!statement.isPrepared()) {
        return fail("prepare delete hospital session");
    }
    statement.bind(1, token);
    return statement.step() == SQLITE_DONE || fail("delete hospital session");
}

bool Database::updateHospitalProfile(int hospitalId, const HospitalRecord& hospital) {
    Statement statement(connection, R"(
        UPDATE hospitals SET name = ?, phone = ?, emergency_phone = ?, hospital_type = ?, license_number = ?,
               available_blood_groups = ?, opening_time = ?, closing_time = ?, working_days = ?,
               website = ?, description = ?, facilities = ?
        WHERE id = ?;
    )");
    if (!statement.isPrepared()) {
        return fail("prepare update hospital");
    }
    statement.bind(1, hospital.name).bind(2, hospital.phone).bind(3, hospital.emergencyPhone)
             .bind(4, hospital.hospitalType).bind(5, hospital.licenseNumber)
             .bind(6, joinList(hospital.bloodGroups)).bind(7, hospital.openingTime)
             .bind(8, hospital.closingTime).bind(9, joinList(hospital.workingDays))
             .bind(10, hospital.website).bind(11, hospital.description)
             .bind(12, joinList(hospital.facilities)).bind(13, hospitalId);
    if (statement.step() != SQLITE_DONE) {
        if (sqlite3_extended_errcode(connection) == SQLITE_CONSTRAINT_UNIQUE) {
            setError("A hospital with this name is already registered.");
            return false;
        }
        return fail("update hospital");
    }
    return true;
}

bool Database::updateHospitalLocation(int hospitalId, const HospitalRecord& hospital) {
    Statement statement(connection, R"(
        UPDATE hospitals SET address = ?, city = ?, state = ?, country = ?, pincode = ?, place_id = ?,
               latitude = ?, longitude = ?
        WHERE id = ?;
    )");
    if (!statement.isPrepared()) {
        return fail("prepare update hospital location");
    }
    statement.bind(1, hospital.address).bind(2, hospital.city).bind(3, hospital.state)
             .bind(4, hospital.country).bind(5, hospital.pincode).bind(6, hospital.placeId)
             .bind(7, hospital.latitude).bind(8, hospital.longitude).bind(9, hospitalId);
    return statement.step() == SQLITE_DONE || fail("update hospital location");
}

bool Database::setHospitalAvailability(int hospitalId, const std::string& state, int closingMinutes,
                                       const std::string& reason) {
    // closes_at is stored only for "closing soon", as an absolute UTC time, so the
    // countdown keeps running correctly without the hospital doing anything.
    Statement statement(connection, R"(
        UPDATE hospitals SET availability_status = ?,
               closes_at = CASE WHEN ? = 'closing_soon' THEN datetime('now', ?) ELSE NULL END,
               closure_reason = ?, status_updated_at = CURRENT_TIMESTAMP
        WHERE id = ?;
    )");
    if (!statement.isPrepared()) {
        return fail("prepare set availability");
    }
    statement.bind(1, state).bind(2, state).bind(3, "+" + std::to_string(closingMinutes) + " minutes")
             .bind(4, reason).bind(5, hospitalId);
    if (statement.step() != SQLITE_DONE) {
        return fail("set hospital availability");
    }
    return true;
}

bool Database::setHospitalVerification(int hospitalId, const std::string& status, const std::string& note) {
    Statement statement(connection, R"(
        UPDATE hospitals SET verification_status = ?, verification_note = ?,
               verified_at = CASE WHEN ? = 'verified' THEN CURRENT_TIMESTAMP ELSE NULL END
        WHERE id = ?;
    )");
    if (!statement.isPrepared()) {
        return fail("prepare set verification");
    }
    statement.bind(1, status).bind(2, note).bind(3, status).bind(4, hospitalId);
    if (statement.step() != SQLITE_DONE) {
        return fail("set verification");
    }
    if (sqlite3_changes(connection) != 1) {
        setError("Hospital was not found.");
        return false;
    }
    return true;
}

bool Database::setAdminByEmail(const std::string& email, bool isAdmin) {
    Statement statement(connection, "UPDATE users SET is_admin = ? WHERE email = ?;");
    if (!statement.isPrepared()) {
        return fail("prepare set admin");
    }
    statement.bind(1, isAdmin ? 1 : 0).bind(2, email);
    if (statement.step() != SQLITE_DONE) {
        return fail("set admin");
    }
    if (sqlite3_changes(connection) != 1) {
        setError("There is no account with the email " + email + ".");
        return false;
    }
    return true;
}

// ---------------- Emergency blood postings ----------------

namespace {
// Columns read by readEmergencyPost, in order. Needs "FROM emergency_posts JOIN hospitals".
const char* EMERGENCY_COLUMNS =
    "emergency_posts.id, hospitals.id, hospitals.name, hospitals.phone, "
    "hospitals.address || CASE WHEN hospitals.city <> '' THEN ', ' || hospitals.city ELSE '' END, "
    "hospitals.latitude, hospitals.longitude, emergency_posts.blood_type, emergency_posts.units, "
    "emergency_posts.urgency, emergency_posts.note, emergency_posts.created_at, emergency_posts.expires_at, "
    "(emergency_posts.closed_at IS NULL AND emergency_posts.expires_at > datetime('now'))";

EmergencyPost readEmergencyPost(const Statement& row) {
    EmergencyPost post;
    post.id = row.columnInt(0);
    post.hospitalId = row.columnInt(1);
    post.hospitalName = row.columnText(2);
    post.hospitalPhone = row.columnText(3);
    post.hospitalAddress = row.columnText(4);
    post.hospitalLatitude = row.columnDouble(5);
    post.hospitalLongitude = row.columnDouble(6);
    post.bloodType = row.columnText(7);
    post.units = row.columnInt(8);
    post.urgency = row.columnText(9);
    post.note = row.columnText(10);
    post.createdAt = row.columnText(11);
    post.expiresAt = row.columnText(12);
    post.active = row.columnInt(13) != 0;
    return post;
}
}  // namespace

std::optional<int> Database::createEmergencyPost(int hospitalId, const std::string& bloodType, int units,
                                                 const std::string& urgency, const std::string& note, int hoursOpen) {
    Statement statement(connection,
        "INSERT INTO emergency_posts (hospital_id, blood_type, units, urgency, note, expires_at) "
        "VALUES (?, ?, ?, ?, ?, datetime('now', ?));");
    if (!statement.isPrepared()) {
        fail("prepare emergency post");
        return std::nullopt;
    }
    statement.bind(1, hospitalId).bind(2, bloodType).bind(3, units).bind(4, urgency).bind(5, note)
             .bind(6, "+" + std::to_string(hoursOpen) + " hours");
    if (statement.step() != SQLITE_DONE) {
        fail("create emergency post");
        return std::nullopt;
    }
    return static_cast<int>(sqlite3_last_insert_rowid(connection));
}

bool Database::closeEmergencyPost(int hospitalId, int postId) {
    Statement statement(connection,
        "UPDATE emergency_posts SET closed_at = CURRENT_TIMESTAMP "
        "WHERE id = ? AND hospital_id = ? AND closed_at IS NULL;");
    if (!statement.isPrepared()) {
        return fail("prepare close emergency post");
    }
    statement.bind(1, postId).bind(2, hospitalId);
    if (statement.step() != SQLITE_DONE) {
        return fail("close emergency post");
    }
    if (sqlite3_changes(connection) != 1) {
        setError("This post was not found or is already closed.");
        return false;
    }
    return true;
}

int Database::countActiveEmergencyPosts(int hospitalId) const {
    Statement statement(connection,
        "SELECT COUNT(*) FROM emergency_posts WHERE hospital_id = ? AND closed_at IS NULL "
        "AND expires_at > datetime('now');");
    if (!statement.isPrepared()) {
        fail("prepare count emergency posts");
        return 0;
    }
    statement.bind(1, hospitalId);
    return statement.step() == SQLITE_ROW ? statement.columnInt(0) : 0;
}

std::vector<EmergencyPost> Database::listHospitalEmergencyPosts(int hospitalId) const {
    std::vector<EmergencyPost> posts;
    const std::string sql = std::string("SELECT ") + EMERGENCY_COLUMNS +
        " FROM emergency_posts JOIN hospitals ON hospitals.id = emergency_posts.hospital_id"
        " WHERE hospitals.id = ? AND emergency_posts.created_at >= datetime('now', '-7 days')"
        " ORDER BY emergency_posts.created_at DESC, emergency_posts.id DESC;";
    Statement statement(connection, sql.c_str());
    if (!statement.isPrepared()) {
        fail("prepare hospital emergency posts");
        return posts;
    }
    statement.bind(1, hospitalId);
    while (statement.step() == SQLITE_ROW) {
        posts.push_back(readEmergencyPost(statement));
    }
    return posts;
}

std::vector<EmergencyPost> Database::listActiveEmergencyPostsNear(double latitude, double longitude,
                                                                  double radiusKm) const {
    std::vector<EmergencyPost> posts;
    const BoundingBox box = boundingBoxAround(latitude, longitude, radiusKm);
    const std::string sql = std::string("SELECT ") + EMERGENCY_COLUMNS +
        " FROM emergency_posts JOIN hospitals ON hospitals.id = emergency_posts.hospital_id"
        " WHERE emergency_posts.closed_at IS NULL AND emergency_posts.expires_at > datetime('now')"
        "   AND hospitals.verification_status = 'verified'"
        "   AND hospitals.availability_status <> 'permanently_closed'"
        "   AND hospitals.latitude BETWEEN ? AND ? AND hospitals.longitude BETWEEN ? AND ?;";
    Statement statement(connection, sql.c_str());
    if (!statement.isPrepared()) {
        fail("prepare emergency posts near");
        return posts;
    }
    statement.bind(1, box.minLatitude).bind(2, box.maxLatitude).bind(3, box.minLongitude).bind(4, box.maxLongitude);
    while (statement.step() == SQLITE_ROW) {
        EmergencyPost post = readEmergencyPost(statement);
        post.distanceKm = haversineKm(latitude, longitude, post.hospitalLatitude, post.hospitalLongitude);
        if (post.distanceKm <= radiusKm) {
            posts.push_back(std::move(post));
        }
    }
    std::sort(posts.begin(), posts.end(), [](const EmergencyPost& a, const EmergencyPost& b) {
        if (a.urgency != b.urgency) {
            return a.urgency == "critical";  // critical first
        }
        return a.distanceKm < b.distanceKm;
    });
    return posts;
}

// ---------------- Reopen alerts ----------------

bool Database::addReopenAlert(int userId, int hospitalId) {
    // Asking again after a notification starts a fresh alert.
    Statement statement(connection,
        "INSERT INTO reopen_alerts (user_id, hospital_id) VALUES (?, ?) "
        "ON CONFLICT(user_id, hospital_id) DO UPDATE SET created_at = CURRENT_TIMESTAMP, notified_at = NULL;");
    if (!statement.isPrepared()) {
        return fail("prepare reopen alert");
    }
    statement.bind(1, userId).bind(2, hospitalId);
    return statement.step() == SQLITE_DONE || fail("add reopen alert");
}

bool Database::removeReopenAlert(int userId, int hospitalId) {
    Statement statement(connection, "DELETE FROM reopen_alerts WHERE user_id = ? AND hospital_id = ?;");
    if (!statement.isPrepared()) {
        return fail("prepare remove reopen alert");
    }
    statement.bind(1, userId).bind(2, hospitalId);
    return statement.step() == SQLITE_DONE || fail("remove reopen alert");
}

std::vector<ReopenAlert> Database::listReopenAlerts(int userId) const {
    std::vector<ReopenAlert> alerts;
    Statement statement(connection, R"(
        SELECT reopen_alerts.hospital_id, hospitals.name, reopen_alerts.created_at,
               COALESCE(reopen_alerts.notified_at, '')
        FROM reopen_alerts JOIN hospitals ON hospitals.id = reopen_alerts.hospital_id
        WHERE reopen_alerts.user_id = ?
          AND (reopen_alerts.notified_at IS NULL OR reopen_alerts.notified_at >= datetime('now', '-7 days'))
        ORDER BY reopen_alerts.created_at DESC;
    )");
    if (!statement.isPrepared()) {
        fail("prepare reopen alerts");
        return alerts;
    }
    statement.bind(1, userId);
    while (statement.step() == SQLITE_ROW) {
        alerts.push_back({statement.columnInt(0), statement.columnText(1), statement.columnText(2),
                          statement.columnText(3)});
    }
    return alerts;
}

bool Database::markReopenAlertNotified(int userId, int hospitalId) {
    Statement statement(connection,
        "UPDATE reopen_alerts SET notified_at = CURRENT_TIMESTAMP "
        "WHERE user_id = ? AND hospital_id = ? AND notified_at IS NULL;");
    if (!statement.isPrepared()) {
        return fail("prepare mark reopen alert");
    }
    statement.bind(1, userId).bind(2, hospitalId);
    return statement.step() == SQLITE_DONE || fail("mark reopen alert");
}

std::string Database::getNotificationsSeenAt(int userId) const {
    Statement statement(connection, "SELECT notifications_seen_at FROM users WHERE id = ?;");
    if (!statement.isPrepared()) {
        fail("prepare seen at");
        return "";
    }
    statement.bind(1, userId);
    return statement.step() == SQLITE_ROW ? statement.columnText(0) : "";
}
