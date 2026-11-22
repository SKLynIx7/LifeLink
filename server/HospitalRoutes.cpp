// Hospital routes: registration, login, profile, location, availability, and the
// hospital lists that receivers and donors see. Availability is always worked out here,
// on the server, from the hospital's settings and the current time (engine/HospitalStatus).

#include "Routes.h"

#include "Http.h"
#include "PasswordHasher.h"
#include "Validation.h"
#include "../db/Database.h"
#include "../engine/BloodType.h"
#include "../engine/HospitalStatus.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <ctime>
#include <string>
#include <vector>

using namespace http;

namespace {
const std::vector<std::string> HOSPITAL_TYPES = {"Government", "Private", "Trust", "NGO", "Other"};
const std::vector<std::string> DAYS = {"Mon", "Tue", "Wed", "Thu", "Fri", "Sat", "Sun"};
const std::vector<std::string> FACILITIES = {"blood_bank", "emergency", "icu", "ambulance",
                                             "laboratory", "pharmacy", "operation_theatre"};
const std::vector<std::string> CLOSURE_REASONS = {"Emergency maintenance", "Blood stock unavailable",
                                                  "Staff unavailability", "Holiday", "Other"};
const std::vector<std::string> BLOOD_GROUP_ORDER = {"A+", "A-", "B+", "B-", "AB+", "AB-", "O+", "O-"};
constexpr double DEFAULT_HOSPITAL_RADIUS_KM = 25.0;
constexpr double MAX_HOSPITAL_RADIUS_KM = 50.0;
const std::string UNAVAILABLE = "This hospital is currently unavailable. Please select another nearby hospital.";
constexpr int MAX_EMERGENCY_UNITS = 20;
constexpr int DEFAULT_EMERGENCY_HOURS = 24;
constexpr int MAX_EMERGENCY_HOURS = 72;
constexpr int MAX_ACTIVE_EMERGENCY_POSTS = 10;
constexpr std::size_t MAX_EMERGENCY_NOTE_LENGTH = 300;

bool contains(const std::vector<std::string>& list, const std::string& value) {
    return std::find(list.begin(), list.end(), value) != list.end();
}

LocalTime localNow() {
    const std::time_t now = std::time(nullptr);
    std::tm local{};
#ifdef _WIN32
    localtime_s(&local, &now);
#else
    localtime_r(&now, &local);
#endif
    return LocalTime{(local.tm_wday + 6) % 7, local.tm_hour * 60 + local.tm_min};  // Monday = 0
}

OpeningHours openingHoursOf(const HospitalRecord& hospital) {
    OpeningHours hours;
    hours.openMinute = parseClockTime(hospital.openingTime).value_or(0);
    hours.closeMinute = parseClockTime(hospital.closingTime).value_or(0);
    for (std::size_t day = 0; day < DAYS.size(); ++day) {
        hours.workingDays[day] = contains(hospital.workingDays, DAYS[day]);
    }
    return hours;
}

json statusJson(const HospitalStatus& status) {
    return {
        {"code", status.code},
        {"badge", status.badge},
        {"message", status.message},
        {"minutes_until_close", status.minutesUntilClose},
        {"accepts_new_donations", status.acceptsNewDonations}
    };
}

// What anyone may see about a hospital. The email and password are never included.
json publicHospitalJson(const HospitalRecord& hospital, const HospitalStatus& status) {
    json result = {
        {"id", hospital.id},
        {"name", hospital.name},
        {"address", hospital.address},
        {"city", hospital.city},
        {"state", hospital.state},
        {"country", hospital.country},
        {"pincode", hospital.pincode},
        {"latitude", hospital.latitude},
        {"longitude", hospital.longitude},
        {"phone", hospital.phone},
        {"emergency_phone", hospital.emergencyPhone},
        {"hospital_type", hospital.hospitalType},
        {"blood_groups", hospital.bloodGroups},
        {"opening_time", hospital.openingTime},
        {"closing_time", hospital.closingTime},
        {"open_24_hours", hospital.openingTime == hospital.closingTime},
        {"working_days", hospital.workingDays},
        {"website", hospital.website},
        {"description", hospital.description},
        {"facilities", hospital.facilities},
        {"closure_reason", hospital.closureReason},
        {"status", statusJson(status)},
        {"map_url", "https://www.openstreetmap.org/?mlat=" + std::to_string(hospital.latitude) +
                    "&mlon=" + std::to_string(hospital.longitude) + "#map=17/" +
                    std::to_string(hospital.latitude) + "/" + std::to_string(hospital.longitude)}
    };
    if (hospital.distanceKm >= 0) {
        result["distance_km"] = std::round(hospital.distanceKm * 10.0) / 10.0;
    }
    return result;
}

std::optional<int> authenticatedHospitalId(const crow::request& request, Database& database) {
    const std::string token = request.get_header_value("X-Hospital-Token");
    if (token.empty() || token.size() > 128) {
        return std::nullopt;
    }
    return database.getHospitalSessionId(token);
}

// A list field such as ["A+", "O-"]: every item must be allowed. Returned in the allowed order.
std::optional<std::vector<std::string>> listField(const json& body, const char* name,
                                                  const std::vector<std::string>& allowed, bool required) {
    const auto field = body.find(name);
    if (field == body.end() || field->is_null()) {
        return required ? std::nullopt : std::optional<std::vector<std::string>>(std::vector<std::string>{});
    }
    if (!field->is_array()) {
        return std::nullopt;
    }
    std::vector<std::string> chosen;
    for (const json& item : *field) {
        if (!item.is_string() || !contains(allowed, item.get<std::string>())) {
            return std::nullopt;
        }
        chosen.push_back(item.get<std::string>());
    }
    std::vector<std::string> ordered;
    for (const std::string& value : allowed) {
        if (contains(chosen, value)) {
            ordered.push_back(value);
        }
    }
    if (required && ordered.empty()) {
        return std::nullopt;
    }
    return ordered;
}

// The editable profile fields (everything except email, password, location and availability).
bool readHospitalProfile(const json& body, HospitalRecord& hospital, std::string& error) {
    const auto name = textField(body, "name", 120);
    const auto phone = textField(body, "phone", 25);
    const auto emergency = optionalTextField(body, "emergency_phone", 25);
    const auto type = textField(body, "hospital_type", 20);
    const auto license = optionalTextField(body, "license_number", 50);
    const auto bloodGroups = listField(body, "blood_groups", BLOOD_GROUP_ORDER, true);
    const auto opening = textField(body, "opening_time", 5);
    const auto closing = textField(body, "closing_time", 5);
    const auto days = listField(body, "working_days", DAYS, true);
    const auto website = optionalTextField(body, "website", 200);
    const auto description = optionalTextField(body, "description", 1000);
    const auto facilities = listField(body, "facilities", FACILITIES, false);

    if (!name || name->size() < 2) {
        error = "Enter the hospital name.";
    } else if (!phone || !Validation::isValidHospitalPhone(*phone)) {
        error = "Enter a valid phone number (10 to 13 digits, e.g. 0135-2654321).";
    } else if (!emergency || (!emergency->empty() && !Validation::isValidHospitalPhone(*emergency))) {
        error = "Enter a valid emergency contact number, or leave it empty.";
    } else if (!type || !contains(HOSPITAL_TYPES, *type)) {
        error = "Choose the hospital type: Government, Private, Trust, NGO or Other.";
    } else if (!license) {
        error = "The licence or registration number can be at most 50 characters.";
    } else if (!bloodGroups) {
        error = "Choose at least one available blood group.";
    } else if (!opening || !parseClockTime(*opening) || !closing || !parseClockTime(*closing)) {
        error = "Enter opening and closing times as HH:MM (24-hour).";
    } else if (!days) {
        error = "Choose at least one working day.";
    } else if (!website || (!website->empty() && !Validation::isValidWebsite(*website))) {
        error = "The website must start with http:// or https://, or leave it empty.";
    } else if (!description) {
        error = "The description can be at most 1000 characters.";
    } else if (!facilities) {
        error = "Choose facilities from the list.";
    }
    if (!error.empty()) {
        return false;
    }
    hospital.name = *name;
    hospital.phone = *phone;
    hospital.emergencyPhone = *emergency;
    hospital.hospitalType = *type;
    hospital.licenseNumber = *license;
    hospital.bloodGroups = *bloodGroups;
    hospital.openingTime = *opening;
    hospital.closingTime = *closing;
    hospital.workingDays = *days;
    hospital.website = *website;
    hospital.description = *description;
    hospital.facilities = *facilities;
    return true;
}

// The address and the map position (from the location picker).
bool readHospitalLocation(const json& body, HospitalRecord& hospital, std::string& error) {
    const auto address = textField(body, "address", 300);
    const auto city = textField(body, "city", 100);
    const auto state = textField(body, "state", 100);
    const auto country = textField(body, "country", 100);
    const auto pincode = textField(body, "pincode", 6);
    const auto placeId = optionalTextField(body, "place_id", 120);
    const auto latitude = numberField(body, "latitude");
    const auto longitude = numberField(body, "longitude");

    if (!address || address->size() < 5) {
        error = "Enter the hospital address.";
    } else if (!city || !state || !country) {
        error = "Enter the city, state and country.";
    } else if (!pincode || !Validation::isValidPincode(*pincode)) {
        error = "Enter a valid 6-digit PIN code.";
    } else if (!latitude || !longitude || !Validation::isValidLatitude(*latitude) ||
               !Validation::isValidLongitude(*longitude)) {
        error = "Select the hospital's exact location on the map.";
    } else if (!placeId) {
        error = "The selected place is not valid. Select the location again.";
    }
    if (!error.empty()) {
        return false;
    }
    hospital.address = *address;
    hospital.city = *city;
    hospital.state = *state;
    hospital.country = *country;
    hospital.pincode = *pincode;
    hospital.placeId = *placeId;
    hospital.latitude = *latitude;
    hospital.longitude = *longitude;
    return true;
}

}  // namespace

json hospitalFullJson(const HospitalRecord& hospital) {
    json result = publicHospitalJson(hospital, currentHospitalStatus(hospital));
    result["email"] = hospital.email;
    result["place_id"] = hospital.placeId;
    result["license_number"] = hospital.licenseNumber;
    result["availability_state"] = hospital.availabilityState;
    result["seconds_until_manual_close"] = hospital.secondsUntilManualClose
        ? json(*hospital.secondsUntilManualClose) : json(nullptr);
    result["verification_status"] = hospital.verificationStatus;
    result["verification_note"] = hospital.verificationNote;
    result["registered_at"] = hospital.registeredAt;
    return result;
}

json emergencyPostJson(const EmergencyPost& post) {
    json result = {
        {"id", post.id},
        {"hospital_id", post.hospitalId},
        {"hospital_name", post.hospitalName},
        {"hospital_phone", post.hospitalPhone},
        {"hospital_address", post.hospitalAddress},
        {"blood_type", post.bloodType},
        {"units", post.units},
        {"urgency", post.urgency},
        {"note", post.note},
        {"created_at", post.createdAt},
        {"expires_at", post.expiresAt},
        {"active", post.active},
        {"map_url", "https://www.openstreetmap.org/?mlat=" + std::to_string(post.hospitalLatitude) +
                    "&mlon=" + std::to_string(post.hospitalLongitude) + "#map=17/" +
                    std::to_string(post.hospitalLatitude) + "/" + std::to_string(post.hospitalLongitude)}
    };
    if (post.distanceKm >= 0) {
        result["distance_km"] = std::round(post.distanceKm * 10.0) / 10.0;
    }
    return result;
}

namespace {

// Checks the logged-in hospital is the one in the URL.
std::optional<crow::response> requireOwnHospital(const crow::request& request, Database& database, int hospitalId) {
    const std::optional<int> loggedIn = authenticatedHospitalId(request, database);
    if (!loggedIn) {
        return errorResponse(401, "Please log in as a hospital.");
    }
    if (*loggedIn != hospitalId) {
        return errorResponse(403, "You can only change your own hospital.");
    }
    return std::nullopt;
}

// A rejected hospital that corrects its details goes back to the administrator.
void sendBackForReviewIfRejected(Database& database, const HospitalRecord& hospital) {
    if (hospital.verificationStatus == "rejected") {
        database.setHospitalVerification(hospital.id, "pending", "");
    }
}

std::string lockoutKey(const std::string& email) {
    return "hospital:" + email;  // kept apart from user logins with the same email
}
}  // namespace

HospitalStatus currentHospitalStatus(const HospitalRecord& hospital) {
    const HospitalState state = parseHospitalState(hospital.availabilityState).value_or(HospitalState::Open);
    return evaluateHospitalStatus(state, hospital.secondsUntilManualClose, openingHoursOf(hospital), localNow());
}

void registerHospitalRoutes(crow::SimpleApp& app, Database& database) {
    // ---------------- Lists for receivers and donors ----------------
    // GET /api/hospitals?latitude=&longitude=&radius_km=&blood_group=&show_closed=1
    // With a point: hospitals within radius_km (default 25, at most 50), nearest first.
    // Hospitals that can take a visit now come first. Permanently closed ones are hidden
    // unless show_closed=1.
    const auto listRoute = [&database](const crow::request& request) {
        const char* latitudeText = request.url_params.get("latitude");
        const char* longitudeText = request.url_params.get("longitude");
        const char* radiusText = request.url_params.get("radius_km");
        const char* bloodGroup = request.url_params.get("blood_group");
        const char* showClosedText = request.url_params.get("show_closed");
        const bool showClosed = showClosedText != nullptr && std::string(showClosedText) == "1";

        std::vector<HospitalRecord> hospitals;
        double radiusKm = DEFAULT_HOSPITAL_RADIUS_KM;
        const bool nearPoint = latitudeText != nullptr || longitudeText != nullptr;
        if (nearPoint) {
            char* end1 = nullptr;
            char* end2 = nullptr;
            const double latitude = latitudeText ? std::strtod(latitudeText, &end1) : 1000.0;
            const double longitude = longitudeText ? std::strtod(longitudeText, &end2) : 1000.0;
            if (!latitudeText || !longitudeText || *end1 != '\0' || *end2 != '\0' ||
                !Validation::isValidLatitude(latitude) || !Validation::isValidLongitude(longitude)) {
                return errorResponse(400, "latitude and longitude must both be valid numbers.");
            }
            if (radiusText != nullptr) {
                char* end = nullptr;
                radiusKm = std::strtod(radiusText, &end);
                if (*end != '\0' || !(radiusKm > 0) || radiusKm > MAX_HOSPITAL_RADIUS_KM) {
                    return errorResponse(400, "radius_km must be more than 0 and at most 50.");
                }
            }
            hospitals = database.listHospitalsNear(latitude, longitude, radiusKm);
        } else {
            hospitals = database.listHospitals();
        }
        if (bloodGroup != nullptr && !isValidBloodType(bloodGroup)) {
            return errorResponse(400, "blood_group must be one of A+, A-, B+, B-, AB+, AB-, O+, O-.");
        }

        struct Entry {
            HospitalRecord hospital;
            HospitalStatus status;
        };
        std::vector<Entry> entries;
        for (HospitalRecord& hospital : hospitals) {
            if (hospital.verificationStatus != "verified") {
                continue;  // waiting for, or refused, approval by an administrator
            }
            const HospitalStatus status = currentHospitalStatus(hospital);
            if (status.code == "permanently_closed" && !showClosed) {
                continue;
            }
            if (bloodGroup != nullptr && !contains(hospital.bloodGroups, bloodGroup)) {
                continue;
            }
            entries.push_back({std::move(hospital), status});
        }
        std::stable_sort(entries.begin(), entries.end(), [](const Entry& a, const Entry& b) {
            if (a.status.acceptsNewDonations != b.status.acceptsNewDonations) {
                return a.status.acceptsNewDonations;
            }
            return a.hospital.distanceKm < b.hospital.distanceKm;
        });

        json list = json::array();
        for (const Entry& entry : entries) {
            list.push_back(publicHospitalJson(entry.hospital, entry.status));
        }
        json response = {{"hospitals", list}, {"show_closed", showClosed}};
        if (nearPoint) {
            response["radius_km"] = radiusKm;
        }
        return jsonResponse(200, response);
    };
    CROW_ROUTE(app, "/api/hospitals").methods(crow::HTTPMethod::GET)(listRoute);
    CROW_ROUTE(app, "/api/hospitals/nearby").methods(crow::HTTPMethod::GET)(listRoute);

    CROW_ROUTE(app, "/api/hospitals/<int>").methods(crow::HTTPMethod::GET)([&database](int hospitalId) {
        const std::optional<HospitalRecord> hospital = database.getHospital(hospitalId);
        if (!hospital || hospital->verificationStatus != "verified") {
            return errorResponse(404, "Hospital was not found.");
        }
        return jsonResponse(200, {{"hospital", publicHospitalJson(*hospital, currentHospitalStatus(*hospital))}});
    });

    // ---------------- Hospital accounts ----------------
    CROW_ROUTE(app, "/api/hospitals/register").methods(crow::HTTPMethod::POST)([&database](const crow::request& request) {
        const std::optional<json> body = readJson(request);
        if (!body) {
            return errorResponse(400, "Request body must be a JSON object.");
        }
        const auto rawEmail = textField(*body, "email", 254);
        const auto password = passwordField(*body, "password");
        if (!rawEmail || !Validation::isValidEmail(Validation::normalizeEmail(*rawEmail))) {
            return errorResponse(400, "Enter a valid official email address.");
        }
        if (!password || !Validation::isStrongPassword(*password)) {
            return errorResponse(400, "Password must be 8 to 128 characters with at least one letter and one number.");
        }

        HospitalRecord hospital;
        hospital.email = Validation::normalizeEmail(*rawEmail);
        std::string error;
        if (!readHospitalProfile(*body, hospital, error) || !readHospitalLocation(*body, hospital, error)) {
            return errorResponse(400, error);
        }
        if (database.findHospitalIdByEmail(hospital.email)) {
            return errorResponse(409, "A hospital with this email is already registered.");
        }

        const std::string passwordHash = PasswordHasher::hashPassword(*password);
        const std::string token = PasswordHasher::createSessionToken();
        if (passwordHash.empty() || token.empty()) {
            return errorResponse(500, "Could not secure the password.");
        }
        const std::optional<int> hospitalId = database.registerHospital(hospital, passwordHash);
        if (!hospitalId) {
            return errorResponse(409, database.getLastError());
        }
        // Registration logs the hospital straight in.
        if (!database.createHospitalSession(*hospitalId, token)) {
            return errorResponse(500, database.getLastError());
        }
        return jsonResponse(201, {{"message", "Hospital registered."}, {"hospital_id", *hospitalId},
                                  {"session_token", token}, {"name", hospital.name}});
    });

    CROW_ROUTE(app, "/api/hospitals/login").methods(crow::HTTPMethod::POST)([&database](const crow::request& request) {
        const std::optional<json> body = readJson(request);
        const auto rawEmail = body ? textField(*body, "email", 254) : std::nullopt;
        const auto password = body ? passwordField(*body, "password") : std::nullopt;
        if (!rawEmail || !password || password->empty() || password->size() > 128) {
            return errorResponse(400, "Email and password are required.");
        }
        const std::string email = Validation::normalizeEmail(*rawEmail);
        if (database.isLockedOut(lockoutKey(email))) {
            return errorResponse(429, "Too many failed attempts. Please try again in " +
                                          std::to_string(LOCKOUT_MINUTES) + " minutes.");
        }
        const std::optional<std::string> storedHash = database.getHospitalPasswordHash(email);
        const std::optional<int> hospitalId = database.findHospitalIdByEmail(email);
        if (!storedHash || !hospitalId || !PasswordHasher::verifyPassword(*password, *storedHash)) {
            database.recordFailedAttempt(lockoutKey(email));
            return errorResponse(401, "Invalid email or password.");
        }
        database.clearFailedAttempts(lockoutKey(email));

        const std::string token = PasswordHasher::createSessionToken();
        if (token.empty() || !database.createHospitalSession(*hospitalId, token)) {
            return errorResponse(500, "Could not create a session.");
        }
        const std::optional<HospitalRecord> hospital = database.getHospital(*hospitalId);
        return jsonResponse(200, {{"session_token", token}, {"hospital_id", *hospitalId},
                                  {"name", hospital ? hospital->name : ""},
                                  {"expires_in_hours", SESSION_LIFETIME_HOURS}});
    });

    CROW_ROUTE(app, "/api/hospitals/logout").methods(crow::HTTPMethod::POST)([&database](const crow::request& request) {
        const std::string token = request.get_header_value("X-Hospital-Token");
        if (!token.empty() && token.size() <= 128) {
            database.deleteHospitalSession(token);
        }
        return jsonResponse(200, {{"message", "Logged out."}});
    });

    CROW_ROUTE(app, "/api/hospital/me").methods(crow::HTTPMethod::GET)([&database](const crow::request& request) {
        const std::optional<int> hospitalId = authenticatedHospitalId(request, database);
        if (!hospitalId) {
            return errorResponse(401, "Please log in as a hospital.");
        }
        const std::optional<HospitalRecord> hospital = database.getHospital(*hospitalId);
        if (!hospital) {
            return errorResponse(404, "Hospital was not found.");
        }
        return jsonResponse(200, {{"hospital", hospitalFullJson(*hospital)}});
    });

    // ---------------- Dashboard changes (own hospital only) ----------------
    CROW_ROUTE(app, "/api/hospitals/<int>/profile").methods(crow::HTTPMethod::PUT)(
        [&database](const crow::request& request, int hospitalId) {
            if (auto denied = requireOwnHospital(request, database, hospitalId)) {
                return std::move(*denied);
            }
            const std::optional<json> body = readJson(request);
            if (!body) {
                return errorResponse(400, "Request body must be a JSON object.");
            }
            std::optional<HospitalRecord> hospital = database.getHospital(hospitalId);
            std::string error;
            if (!hospital || !readHospitalProfile(*body, *hospital, error)) {
                return errorResponse(400, error.empty() ? "Hospital was not found." : error);
            }
            if (!database.updateHospitalProfile(hospitalId, *hospital)) {
                return errorResponse(409, database.getLastError());
            }
            sendBackForReviewIfRejected(database, *hospital);
            return jsonResponse(200, {{"message", "Hospital details saved."},
                                      {"hospital", hospitalFullJson(*database.getHospital(hospitalId))}});
        });

    CROW_ROUTE(app, "/api/hospitals/<int>/location").methods(crow::HTTPMethod::PUT)(
        [&database](const crow::request& request, int hospitalId) {
            if (auto denied = requireOwnHospital(request, database, hospitalId)) {
                return std::move(*denied);
            }
            const std::optional<json> body = readJson(request);
            if (!body) {
                return errorResponse(400, "Request body must be a JSON object.");
            }
            std::optional<HospitalRecord> hospital = database.getHospital(hospitalId);
            std::string error;
            if (!hospital || !readHospitalLocation(*body, *hospital, error)) {
                return errorResponse(400, error.empty() ? "Hospital was not found." : error);
            }
            if (!database.updateHospitalLocation(hospitalId, *hospital)) {
                return errorResponse(500, database.getLastError());
            }
            sendBackForReviewIfRejected(database, *hospital);
            return jsonResponse(200, {{"message", "Hospital location saved."},
                                      {"hospital", hospitalFullJson(*database.getHospital(hospitalId))}});
        });

    // Body: {"status": "open" | "temporarily_closed" | "permanently_closed",
    //        "closure_type": "closing_soon" | "closed_now"   (with temporarily_closed),
    //        "closing_minutes": 1..720                       (with closing_soon),
    //        "reason": "" or one of CLOSURE_REASONS}
    CROW_ROUTE(app, "/api/hospitals/<int>/availability").methods(crow::HTTPMethod::PUT)(
        [&database](const crow::request& request, int hospitalId) {
            if (auto denied = requireOwnHospital(request, database, hospitalId)) {
                return std::move(*denied);
            }
            const std::optional<json> body = readJson(request);
            const auto status = body ? textField(*body, "status", 30) : std::nullopt;
            const auto reason = body ? optionalTextField(*body, "reason", 40) : std::nullopt;
            if (!status || (*status != "open" && *status != "temporarily_closed" && *status != "permanently_closed")) {
                return errorResponse(400, "status must be open, temporarily_closed or permanently_closed.");
            }
            if (!reason || (!reason->empty() && !contains(CLOSURE_REASONS, *reason))) {
                return errorResponse(400, "Choose a closure reason from the list, or leave it empty.");
            }

            std::string state = *status;
            int closingMinutes = 0;
            if (*status == "temporarily_closed") {
                const auto type = textField(*body, "closure_type", 20);
                if (!type || (*type != "closing_soon" && *type != "closed_now")) {
                    return errorResponse(400, "Choose whether the hospital is closing soon or closed now.");
                }
                if (*type == "closing_soon") {
                    const auto minutes = intField(*body, "closing_minutes");
                    if (!minutes || !isValidClosingSoonMinutes(*minutes)) {
                        return errorResponse(400, "Minutes until closing must be a whole number from " +
                                                      std::to_string(MIN_CLOSING_SOON_MINUTES) + " to " +
                                                      std::to_string(MAX_CLOSING_SOON_MINUTES) + " (12 hours).");
                    }
                    state = "closing_soon";
                    closingMinutes = *minutes;
                }
            }
            if (!database.setHospitalAvailability(hospitalId, state, closingMinutes, *status == "open" ? "" : *reason)) {
                return errorResponse(500, database.getLastError());
            }
            const std::optional<HospitalRecord> hospital = database.getHospital(hospitalId);
            return jsonResponse(200, {{"message", "Availability updated."}, {"hospital", hospitalFullJson(*hospital)}});
        });

    // ---------------- Emergency blood postings (own hospital only) ----------------
    // Body: {"blood_type": "O-", "units": 1-20, "urgency": "critical" | "urgent",
    //        "note": "" (up to 300 characters), "hours": 1-72 (how long it stays up, default 24)}
    CROW_ROUTE(app, "/api/hospitals/<int>/emergencies").methods(crow::HTTPMethod::POST)(
        [&database](const crow::request& request, int hospitalId) {
            if (auto denied = requireOwnHospital(request, database, hospitalId)) {
                return std::move(*denied);
            }
            const std::optional<HospitalRecord> hospital = database.getHospital(hospitalId);
            if (!hospital) {
                return errorResponse(404, "Hospital was not found.");
            }
            if (hospital->verificationStatus != "verified") {
                return errorResponse(403, "Emergency posts are available once an administrator has approved your hospital.");
            }
            if (hospital->availabilityState == "permanently_closed") {
                return errorResponse(409, "A permanently closed hospital cannot post blood requirements.");
            }
            const std::optional<json> body = readJson(request);
            const auto bloodType = body ? textField(*body, "blood_type", 3) : std::nullopt;
            const auto units = body ? intField(*body, "units") : std::nullopt;
            const auto urgency = body ? textField(*body, "urgency", 10) : std::nullopt;
            const auto note = body ? optionalTextField(*body, "note", MAX_EMERGENCY_NOTE_LENGTH) : std::nullopt;
            std::optional<int> hours = DEFAULT_EMERGENCY_HOURS;
            if (body && body->contains("hours")) {
                hours = intField(*body, "hours");
            }
            if (!bloodType || !isValidBloodType(*bloodType)) {
                return errorResponse(400, "Choose the blood group needed.");
            }
            if (!units || *units < 1 || *units > MAX_EMERGENCY_UNITS) {
                return errorResponse(400, "Units needed must be from 1 to " + std::to_string(MAX_EMERGENCY_UNITS) + ".");
            }
            if (!urgency || (*urgency != "critical" && *urgency != "urgent")) {
                return errorResponse(400, "Urgency must be critical or urgent.");
            }
            if (!note) {
                return errorResponse(400, "The note can be at most " + std::to_string(MAX_EMERGENCY_NOTE_LENGTH) + " characters.");
            }
            if (!hours || *hours < 1 || *hours > MAX_EMERGENCY_HOURS) {
                return errorResponse(400, "The post can stay up for 1 to " + std::to_string(MAX_EMERGENCY_HOURS) + " hours.");
            }
            if (database.countActiveEmergencyPosts(hospitalId) >= MAX_ACTIVE_EMERGENCY_POSTS) {
                return errorResponse(409, "You already have " + std::to_string(MAX_ACTIVE_EMERGENCY_POSTS) +
                                              " active posts. Close one before adding another.");
            }
            const std::optional<int> postId =
                database.createEmergencyPost(hospitalId, *bloodType, *units, *urgency, *note, *hours);
            if (!postId) {
                return errorResponse(500, database.getLastError());
            }
            return jsonResponse(201, {{"message", "Posted. Compatible donors within 25 km are alerted."},
                                      {"post_id", *postId}});
        });

    CROW_ROUTE(app, "/api/hospital/emergencies").methods(crow::HTTPMethod::GET)([&database](const crow::request& request) {
        const std::optional<int> hospitalId = authenticatedHospitalId(request, database);
        if (!hospitalId) {
            return errorResponse(401, "Please log in as a hospital.");
        }
        json posts = json::array();
        for (const EmergencyPost& post : database.listHospitalEmergencyPosts(*hospitalId)) {
            posts.push_back(emergencyPostJson(post));
        }
        return jsonResponse(200, {{"posts", posts}});
    });

    CROW_ROUTE(app, "/api/hospitals/<int>/emergencies/<int>/close").methods(crow::HTTPMethod::POST)(
        [&database](const crow::request& request, int hospitalId, int postId) {
            if (auto denied = requireOwnHospital(request, database, hospitalId)) {
                return std::move(*denied);
            }
            if (!database.closeEmergencyPost(hospitalId, postId)) {
                return errorResponse(404, database.getLastError());
            }
            return jsonResponse(200, {{"message", "Post closed."}});
        });
}

std::string hospitalUnavailableMessage() {
    return UNAVAILABLE;
}
