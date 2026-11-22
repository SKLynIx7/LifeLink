#include "Routes.h"

#include "Http.h"
#include "PasswordHasher.h"
#include "Validation.h"
#include "../db/Database.h"
#include "../engine/BloodType.h"
#include "../engine/Eligibility.h"
#include "../engine/MatchRequest.h"
#include "../engine/RadiusSearch.h"

#include <nlohmann/json.hpp>

#include <cmath>
#include <fstream>
#include <unordered_map>
#include <unordered_set>
#include <iterator>
#include <limits>
#include <optional>
#include <string>

using namespace http;

namespace {

// A location from the map picker or the area list:
//   required: "area" (short label), "latitude", "longitude"
//   optional: "formatted_address", "city", "state", "country", "postal_code", "place_id"
// On a problem, `error` says what is wrong and nullopt is returned.
std::optional<UserLocation> readLocation(const json& body, std::string& error) {
    const auto area = textField(body, "area", 80);
    const auto latitude = numberField(body, "latitude");
    const auto longitude = numberField(body, "longitude");
    if (!area || !latitude || !longitude) {
        error = "Choose your location on the map or pick your area.";
        return std::nullopt;
    }
    if (!Validation::isValidLatitude(*latitude) || !Validation::isValidLongitude(*longitude)) {
        error = "That location is not valid. Latitude must be -90 to 90 and longitude -180 to 180.";
        return std::nullopt;
    }

    const auto address = optionalTextField(body, "formatted_address", 300);
    const auto city = optionalTextField(body, "city", 100);
    const auto state = optionalTextField(body, "state", 100);
    const auto country = optionalTextField(body, "country", 100);
    const auto postalCode = optionalTextField(body, "postal_code", 20);
    const auto placeId = optionalTextField(body, "place_id", 120);
    if (!address || !city || !state || !country || !postalCode || !placeId) {
        error = "Some address details are too long or not text. Choose the location again.";
        return std::nullopt;
    }
    return UserLocation{*area, *latitude, *longitude, address->empty() ? *area : *address,
                        *city, *state, *country, *postalCode, *placeId};
}

json locationJson(const UserLocation& location) {
    return {
        {"area", location.area},
        {"latitude", location.latitude},
        {"longitude", location.longitude},
        {"formatted_address", location.address},
        {"city", location.city},
        {"state", location.state},
        {"country", location.country},
        {"postal_code", location.postalCode},
        {"place_id", location.placeId}
    };
}

std::optional<UserAccount> getAuthenticatedUser(const crow::request& request, Database& database) {
    const std::string token = request.get_header_value("X-Session-Token");
    if (token.empty() || token.size() > 128) {
        return std::nullopt;
    }

    const std::optional<int> userId = database.getSessionUserId(token);
    return userId.has_value() ? database.findUserById(*userId) : std::nullopt;
}

// Public donor details. The phone number is deliberately left out: it is private
// until the donor has accepted an invitation.
json donorJson(const Donor& donor, std::size_t rank) {
    return {
        {"id", donor.getId()},
        {"rank", rank},
        {"name", donor.getName()},
        {"blood_type", donor.getBloodType()},
        {"distance_km", std::round(donor.getDistanceKm() * 10.0) / 10.0},
        {"reliability_score", donor.getReliabilityScore()},
        {"priority_score", calculatePriorityScore(donor)},
        {"recommended", rank <= RECOMMENDED_DONOR_COUNT},
        {"low_reliability", isLowReliability(donor)}
    };
}

// The ranked list of donors the receiver can still invite, best first (heap order).
json candidatesJson(const MatchRequest& match) {
    json candidates = json::array();
    std::size_t rank = 0;
    for (const Donor& donor : match.getRankedDonors()) {
        candidates.push_back(donorJson(donor, ++rank));
    }
    return candidates;
}

constexpr double EMERGENCY_ALERT_RADIUS_KM = 25.0;

// Urgent posts a donor should hear about: they are registered, available and allowed to
// donate today (90-day rule), the post needs a blood group they can give to, and the
// hospital is within 25 km of their saved location.
std::vector<EmergencyPost> urgentPostsFor(Database& database, const UserAccount& user) {
    std::vector<EmergencyPost> matching;
    if (!user.isDonor) {
        return matching;
    }
    const json profile = database.getDonorProfile(user.id);
    if (profile.is_null() || !profile["available"].get<bool>() || profile["can_donate_again_in_days"].get<int>() > 0) {
        return matching;
    }
    for (EmergencyPost& post : database.listActiveEmergencyPostsNear(user.latitude, user.longitude, EMERGENCY_ALERT_RADIUS_KM)) {
        if (canDonateTo(user.bloodType, post.bloodType)) {
            matching.push_back(std::move(post));
        }
    }
    return matching;
}

// Marks alerts whose hospital can take visits again (by its own setting or its opening
// hours), then returns the user's alerts. Checked whenever the bell refreshes.
std::vector<ReopenAlert> checkReopenAlerts(Database& database, int userId) {
    for (const ReopenAlert& alert : database.listReopenAlerts(userId)) {
        if (!alert.notifiedAt.empty()) {
            continue;
        }
        const std::optional<HospitalRecord> hospital = database.getHospital(alert.hospitalId);
        if (hospital && hospital->verificationStatus == "verified" &&
            currentHospitalStatus(*hospital).acceptsNewDonations) {
            database.markReopenAlertNotified(userId, alert.hospitalId);
        }
    }
    return database.listReopenAlerts(userId);
}

std::optional<UserAccount> requireAdmin(const crow::request& request, Database& database, crow::response& error) {
    const std::optional<UserAccount> user = getAuthenticatedUser(request, database);
    if (!user) {
        error = errorResponse(401, "Please log in.");
        return std::nullopt;
    }
    if (!user->isAdmin) {
        error = errorResponse(403, "Only administrators can do this.");
        return std::nullopt;
    }
    return user;
}

const std::string INVALID_RADIUS = "radius_km must be 5, 10, 15, 20, 25 or \"auto\" (25 km is the maximum).";

// ?radius_km=auto|5|10|15|20|25 on the URL. Missing means auto-expand.
std::optional<RadiusChoice> radiusFromQuery(const crow::request& request) {
    const char* value = request.url_params.get("radius_km");
    return parseRadiusChoice(value == nullptr ? "" : value);
}

// "radius_km": 10 or "radius_km": "auto" in a JSON body. Missing means auto-expand.
std::optional<RadiusChoice> radiusFromBody(const json& body) {
    const auto field = body.find("radius_km");
    if (field == body.end() || field->is_null()) {
        return parseRadiusChoice("");
    }
    if (field->is_string()) {
        return parseRadiusChoice(field->get<std::string>());
    }
    if (field->is_number_integer()) {
        return parseRadiusChoice(std::to_string(field->get<long long>()));
    }
    return std::nullopt;
}

// What the receiver sees about the search: the radius used and the steps tried.
json radiusSearchJson(const RadiusSearchResult& search) {
    return {
        {"radius_km", search.match.getSearchRadiusKm()},
        {"auto_expand", search.autoExpanded},
        {"radii_tried", search.radiiTried},
        {"max_radius_km", MAX_SEARCH_RADIUS_KM},
        {"candidates", candidatesJson(search.match)}
    };
}

// Loads a request and checks that the logged-in receiver owns it.
// On failure, `error` holds the response to send.
std::optional<BloodRequestRecord> loadOwnedRequest(Database& database, const UserAccount& user,
                                                   int requestId, crow::response& error) {
    const std::optional<BloodRequestRecord> record = database.getBloodRequest(requestId);
    // "Not found" for someone else's request too, so request IDs cannot be probed.
    if (!record || record->receiverId != user.id) {
        error = errorResponse(404, "Blood request was not found.");
        return std::nullopt;
    }
    return record;
}

json eligibilityQuestionsJson() {
    json questions = json::array();
    for (const EligibilityQuestion& question : getEligibilityQuestions()) {
        questions.push_back({{"id", question.id}, {"text", question.text}});
    }
    return questions;
}

// Answers must be {"question_id": true/false, ...}.
std::optional<std::unordered_map<std::string, bool>> readAnswers(const json& body) {
    const auto field = body.find("answers");
    if (field == body.end() || !field->is_object()) {
        return std::nullopt;
    }
    std::unordered_map<std::string, bool> answers;
    for (auto entry = field->begin(); entry != field->end(); ++entry) {
        if (!entry.value().is_boolean()) {
            return std::nullopt;
        }
        answers[entry.key()] = entry.value().get<bool>();
    }
    return answers;
}

std::string invitationClosedMessage(const std::string& status) {
    if (status == InvitationStatus::CANCELLED) {
        return "This request was already accepted by another donor or cancelled by the receiver.";
    }
    if (status == InvitationStatus::EXPIRED) {
        return "This invitation expired before it was answered.";
    }
    return "You have already answered this invitation.";
}

const std::string TOO_MANY_ATTEMPTS =
    "Too many failed attempts. Please try again in " + std::to_string(LOCKOUT_MINUTES) + " minutes.";
}

void registerRoutes(crow::SimpleApp& app, Database& database, const std::string& webRoot) {
    registerHospitalRoutes(app, database);

    CROW_ROUTE(app, "/")([webRoot]() {
        return staticFileResponse(webRoot + "/index.html", "text/html; charset=utf-8");
    });

    // Only these files are served, so nothing else in the folder can be downloaded.
    struct WebFile {
        const char* name;
        const char* contentType;
    };
    static const WebFile webFiles[] = {
        {"index.html", "text/html; charset=utf-8"},
        {"login.html", "text/html; charset=utf-8"},
        {"dashboard.html", "text/html; charset=utf-8"},
        {"register.html", "text/html; charset=utf-8"},
        {"forgot.html", "text/html; charset=utf-8"},
        {"profile.html", "text/html; charset=utf-8"},
        {"receiver.html", "text/html; charset=utf-8"},
        {"donor.html", "text/html; charset=utf-8"},
        {"styles.css", "text/css; charset=utf-8"},
        {"app.js", "application/javascript; charset=utf-8"},
        {"auth.js", "application/javascript; charset=utf-8"},
        {"dashboard.js", "application/javascript; charset=utf-8"},
        {"profile.js", "application/javascript; charset=utf-8"},
        {"receiver.js", "application/javascript; charset=utf-8"},
        {"donor.js", "application/javascript; charset=utf-8"},
        {"location-picker.js", "application/javascript; charset=utf-8"},
        {"hospitals.html", "text/html; charset=utf-8"},
        {"hospital-login.html", "text/html; charset=utf-8"},
        {"hospital-register.html", "text/html; charset=utf-8"},
        {"hospital-dashboard.html", "text/html; charset=utf-8"},
        {"hospital-list.js", "application/javascript; charset=utf-8"},
        {"hospital.js", "application/javascript; charset=utf-8"},
        {"admin.html", "text/html; charset=utf-8"},
        {"admin.js", "application/javascript; charset=utf-8"},
        // Leaflet map library (BSD-2-Clause), served from here so no CDN is needed.
        {"vendor/leaflet/leaflet.js", "application/javascript; charset=utf-8"},
        {"vendor/leaflet/leaflet.css", "text/css; charset=utf-8"},
    };
    for (const WebFile& file : webFiles) {
        const std::string path = webRoot + "/" + file.name;
        const std::string contentType = file.contentType;
        app.route_dynamic(std::string("/") + file.name)([path, contentType]() {
            return staticFileResponse(path, contentType.c_str());
        });
    }

    CROW_ROUTE(app, "/ping")([]() {
        return jsonResponse(200, {{"status", "ok"}});
    });

    // One account can both request blood and donate. Donor details are added
    // later with /api/donor/profile, the first time the user opens "Donate".
    CROW_ROUTE(app, "/api/register").methods(crow::HTTPMethod::POST)([&database](const crow::request& request) {
        const std::optional<json> body = readJson(request);
        if (!body.has_value()) {
            return errorResponse(400, "Request body must be a JSON object.");
        }

        const auto name = textField(*body, "name", 100);
        const auto rawEmail = textField(*body, "email", 254);
        const auto password = passwordField(*body, "password");
        const auto question = textField(*body, "security_question", 200);
        const auto answer = textField(*body, "security_answer", 100);
        const auto bloodType = textField(*body, "blood_type", 3);
        if (!name || !rawEmail || !password || !question || !answer) {
            return errorResponse(400, "Name, email, password, security question and answer are required.");
        }
        if (!bloodType || !isValidBloodType(*bloodType)) {
            return errorResponse(400, "Choose your blood group.");
        }
        std::string locationError;
        const std::optional<UserLocation> location = readLocation(*body, locationError);
        if (!location) {
            return errorResponse(400, locationError);
        }

        const std::string email = Validation::normalizeEmail(*rawEmail);
        if (!Validation::isValidEmail(email)) {
            return errorResponse(400, "Enter a valid email address.");
        }
        if (!Validation::isValidPassword(*password)) {
            return errorResponse(400, "Password must be 8 to 128 characters long.");
        }
        if (database.findUserByEmail(email).has_value()) {
            return errorResponse(409, "An account with this email already exists.");
        }

        const std::string passwordHash = PasswordHasher::hashPassword(*password);
        const std::string answerHash = PasswordHasher::hashPassword(Validation::normalizeAnswer(*answer));
        if (passwordHash.empty() || answerHash.empty()) {
            return errorResponse(500, "Could not secure the password.");
        }

        const std::optional<int> userId = database.registerUser(*name, email, passwordHash, *question, answerHash,
                                                                 *bloodType, *location);
        if (!userId.has_value()) {
            return errorResponse(409, database.getLastError());
        }
        return jsonResponse(201, {{"message", "Registration successful."}, {"user_id", *userId}});
    });

    // Becoming a donor. Blood group and location were given at registration,
    // so this only adds the phone number and the last donation date.
    // Body: {"phone": "9876543210", "last_donation_date": "2026-05-01" (optional)}
    CROW_ROUTE(app, "/api/donor/profile").methods(crow::HTTPMethod::POST)([&database](const crow::request& request) {
        const std::optional<UserAccount> user = getAuthenticatedUser(request, database);
        if (!user) {
            return errorResponse(401, "Please log in.");
        }
        if (user->isDonor) {
            return errorResponse(409, "You have already set up your donor profile.");
        }
        const std::optional<json> body = readJson(request);
        if (!body) {
            return errorResponse(400, "Request body must be a JSON object.");
        }

        const auto phone = textField(*body, "phone", 20);
        if (!phone || !Validation::isValidPhone(*phone)) {
            return errorResponse(400, "Enter a valid 10-digit mobile number.");
        }

        std::optional<std::string> lastDonationDate;
        const auto dateField = body->find("last_donation_date");
        if (dateField != body->end() && !dateField->is_null()) {
            lastDonationDate = textField(*body, "last_donation_date", 10);
            if (!lastDonationDate || !Validation::isValidPastDate(*lastDonationDate)) {
                return errorResponse(400, "Last donation date must be a past date in YYYY-MM-DD format.");
            }
        }

        if (!database.createDonorProfile(user->id, *phone, lastDonationDate)) {
            return errorResponse(400, database.getLastError());
        }
        return jsonResponse(201, {{"message", "You are now registered as a donor."},
                                  {"donor_profile", database.getDonorProfile(user->id)}});
    });

    // The bell: what needs the user's attention as a donor or as a receiver.
    // The bell: what needs the user's attention as a donor or as a receiver, plus urgent
    // hospital needs nearby (for donors who can give now) and hospitals that have reopened.
    CROW_ROUTE(app, "/api/notifications").methods(crow::HTTPMethod::GET)([&database](const crow::request& request) {
        const std::optional<UserAccount> user = getAuthenticatedUser(request, database);
        if (!user) {
            return errorResponse(401, "Please log in.");
        }
        database.expireOldInvitations();
        json data = database.getNotifications(user->id);
        const std::string seenAt = database.getNotificationsSeenAt(user->id);
        int unread = data["unread"].get<int>();
        json extra = json::array();

        for (const EmergencyPost& post : urgentPostsFor(database, *user)) {
            // Both are UTC "YYYY-MM-DD HH:MM:SS"; the same second counts as new, so nothing is missed.
            const bool isNew = post.createdAt >= seenAt;
            unread += isNew ? 1 : 0;
            extra.push_back({{"type", "emergency"}, {"post_id", post.id}, {"hospital_name", post.hospitalName},
                             {"blood_type", post.bloodType}, {"units", post.units}, {"urgency", post.urgency},
                             {"distance_km", std::round(post.distanceKm * 10.0) / 10.0},
                             {"at", post.createdAt}, {"unread", isNew}});
        }
        for (const ReopenAlert& alert : checkReopenAlerts(database, user->id)) {
            if (alert.notifiedAt.empty()) {
                continue;  // still closed
            }
            const bool isNew = alert.notifiedAt >= seenAt;
            unread += isNew ? 1 : 0;
            extra.push_back({{"type", "reopened"}, {"hospital_id", alert.hospitalId},
                             {"hospital_name", alert.hospitalName}, {"at", alert.notifiedAt}, {"unread", isNew}});
        }
        for (const json& item : data["items"]) {
            extra.push_back(item);
        }
        return jsonResponse(200, {{"items", extra}, {"unread", unread}});
    });

    // Opening the bell marks "a donor accepted" updates as seen.
    // Waiting invitations stay marked until the donor answers them.
    CROW_ROUTE(app, "/api/notifications/seen").methods(crow::HTTPMethod::POST)([&database](const crow::request& request) {
        const std::optional<UserAccount> user = getAuthenticatedUser(request, database);
        if (!user) {
            return errorResponse(401, "Please log in.");
        }
        if (!database.markNotificationsSeen(user->id)) {
            return errorResponse(500, database.getLastError());
        }
        return jsonResponse(200, {{"message", "Seen."}});
    });

    CROW_ROUTE(app, "/api/login").methods(crow::HTTPMethod::POST)([&database](const crow::request& request) {
        const std::optional<json> body = readJson(request);
        const auto rawEmail = body ? textField(*body, "email", 254) : std::nullopt;
        const auto password = body ? passwordField(*body, "password") : std::nullopt;
        if (!rawEmail || !password || password->empty() || password->size() > 128) {
            return errorResponse(400, "Email and password are required.");
        }

        const std::string email = Validation::normalizeEmail(*rawEmail);
        if (database.isLockedOut(email)) {
            return errorResponse(429, TOO_MANY_ATTEMPTS);
        }

        const std::optional<std::string> storedHash = database.getPasswordHash(email);
        const std::optional<UserAccount> user = database.findUserByEmail(email);
        if (!storedHash || !user || !PasswordHasher::verifyPassword(*password, *storedHash)) {
            database.recordFailedAttempt(email);
            return errorResponse(401, "Invalid email or password.");
        }
        database.clearFailedAttempts(email);

        const std::string token = PasswordHasher::createSessionToken();
        if (token.empty() || !database.createSession(user->id, token)) {
            return errorResponse(500, "Could not create a session.");
        }

        return jsonResponse(200, {
            {"session_token", token},
            {"user_id", user->id},
            {"is_donor", user->isDonor},
            {"name", user->name},
            {"expires_in_hours", SESSION_LIFETIME_HOURS}
        });
    });

    // Profile page. Body: {"name", "blood_type", "phone" (donors only), and the location fields
    // read by readLocation: "area", "latitude", "longitude", "formatted_address", "city", ...}
    CROW_ROUTE(app, "/api/profile").methods(crow::HTTPMethod::POST)([&database](const crow::request& request) {
        const std::optional<UserAccount> user = getAuthenticatedUser(request, database);
        if (!user) {
            return errorResponse(401, "Please log in.");
        }
        const std::optional<json> body = readJson(request);
        if (!body) {
            return errorResponse(400, "Request body must be a JSON object.");
        }

        const auto name = textField(*body, "name", 100);
        const auto bloodType = textField(*body, "blood_type", 3);
        if (!name) {
            return errorResponse(400, "Enter your name.");
        }
        if (!bloodType || !isValidBloodType(*bloodType)) {
            return errorResponse(400, "Choose your blood group.");
        }
        std::string locationError;
        const std::optional<UserLocation> location = readLocation(*body, locationError);
        if (!location) {
            return errorResponse(400, locationError);
        }

        std::optional<std::string> phone;
        if (user->isDonor) {
            phone = textField(*body, "phone", 20);
            if (!phone || !Validation::isValidPhone(*phone)) {
                return errorResponse(400, "Enter a valid 10-digit mobile number.");
            }
        }

        Transaction transaction(database);
        if (!transaction.ok() ||
            !database.updateProfile(user->id, *name, *bloodType, *location) ||
            (phone && !database.updateDonorPhone(user->id, *phone)) || !transaction.commit()) {
            return errorResponse(400, database.getLastError());
        }
        return jsonResponse(200, {{"message", "Your details have been saved."}, {"name", *name}});
    });

    // Body: {"current_password", "new_password"}. Other devices are logged out;
    // this browser gets a fresh session so it stays logged in.
    CROW_ROUTE(app, "/api/change-password").methods(crow::HTTPMethod::POST)([&database](const crow::request& request) {
        const std::optional<UserAccount> user = getAuthenticatedUser(request, database);
        if (!user) {
            return errorResponse(401, "Please log in.");
        }
        const std::optional<json> body = readJson(request);
        const auto current = body ? passwordField(*body, "current_password") : std::nullopt;
        const auto next = body ? passwordField(*body, "new_password") : std::nullopt;
        if (!current || !next) {
            return errorResponse(400, "Enter your current password and a new password.");
        }
        if (!Validation::isValidPassword(*next)) {
            return errorResponse(400, "New password must be 8 to 128 characters long.");
        }
        if (database.isLockedOut(user->email)) {
            return errorResponse(429, TOO_MANY_ATTEMPTS);
        }
        const std::optional<std::string> storedHash = database.getPasswordHash(user->email);
        if (!storedHash || !PasswordHasher::verifyPassword(*current, *storedHash)) {
            database.recordFailedAttempt(user->email);
            return errorResponse(400, "Your current password is not correct.");
        }

        const std::string newHash = PasswordHasher::hashPassword(*next);
        const std::string token = PasswordHasher::createSessionToken();
        if (newHash.empty() || token.empty() || !database.updatePassword(user->email, newHash) ||
            !database.createSession(user->id, token)) {
            return errorResponse(500, "Could not change the password.");
        }
        database.clearFailedAttempts(user->email);
        return jsonResponse(200, {{"message", "Password changed."}, {"session_token", token}});
    });

    CROW_ROUTE(app, "/api/logout").methods(crow::HTTPMethod::POST)([&database](const crow::request& request) {
        const std::string token = request.get_header_value("X-Session-Token");
        if (token.empty()) {
            return errorResponse(401, "Please log in.");
        }
        if (!database.deleteSession(token)) {
            return errorResponse(500, database.getLastError());
        }
        return jsonResponse(200, {{"message", "Logged out."}});
    });

    // POST (not GET) so the email never appears in URLs or server logs.
    CROW_ROUTE(app, "/api/security-question").methods(crow::HTTPMethod::POST)([&database](const crow::request& request) {
        const std::optional<json> body = readJson(request);
        const auto rawEmail = body ? textField(*body, "email", 254) : std::nullopt;
        if (!rawEmail) {
            return errorResponse(400, "Email is required.");
        }

        const std::optional<std::string> question =
            database.getSecurityQuestion(Validation::normalizeEmail(*rawEmail));
        if (!question) {
            return errorResponse(404, "No account found for this email.");
        }
        return jsonResponse(200, {{"security_question", *question}});
    });

    CROW_ROUTE(app, "/api/forgot-password").methods(crow::HTTPMethod::POST)([&database](const crow::request& request) {
        const std::optional<json> body = readJson(request);
        const auto rawEmail = body ? textField(*body, "email", 254) : std::nullopt;
        const auto answer = body ? textField(*body, "security_answer", 100) : std::nullopt;
        const auto newPassword = body ? passwordField(*body, "new_password") : std::nullopt;
        if (!rawEmail || !answer || !newPassword) {
            return errorResponse(400, "Email, security answer and new password are required.");
        }
        if (!Validation::isValidPassword(*newPassword)) {
            return errorResponse(400, "New password must be 8 to 128 characters long.");
        }

        const std::string email = Validation::normalizeEmail(*rawEmail);
        if (database.isLockedOut(email)) {
            return errorResponse(429, TOO_MANY_ATTEMPTS);
        }

        const std::optional<std::string> answerHash = database.getSecurityAnswerHash(email);
        if (!answerHash ||
            !PasswordHasher::verifyPassword(Validation::normalizeAnswer(*answer), *answerHash)) {
            database.recordFailedAttempt(email);
            return errorResponse(401, "Email or security answer is incorrect.");
        }

        const std::string passwordHash = PasswordHasher::hashPassword(*newPassword);
        if (passwordHash.empty() || !database.updatePassword(email, passwordHash)) {
            return errorResponse(500, "Could not update password.");
        }
        database.clearFailedAttempts(email);

        return jsonResponse(200, {{"message", "Password updated. Please log in with your new password."}});
    });

    CROW_ROUTE(app, "/api/me").methods(crow::HTTPMethod::GET)([&database](const crow::request& request) {
        const std::optional<UserAccount> user = getAuthenticatedUser(request, database);
        if (!user) {
            return errorResponse(401, "Please log in.");
        }
        json response = {{"user_id", user->id}, {"name", user->name}, {"email", user->email},
                         {"blood_type", user->bloodType}, {"area", user->area},
                         {"latitude", user->latitude}, {"longitude", user->longitude},
                         {"is_donor", user->isDonor}, {"is_admin", user->isAdmin},
                         {"donor_profile", nullptr}, {"location", nullptr}};
        if (user->isDonor) {
            response["donor_profile"] = database.getDonorProfile(user->id);
        }
        if (const std::optional<UserLocation> location = database.getLocation(user->id)) {
            response["location"] = locationJson(*location);
        }
        return jsonResponse(200, response);
    });

    // ---------------- Saved location (map picker) ----------------
    // The saved location is what distances to hospitals are measured from.

    CROW_ROUTE(app, "/api/location/me").methods(crow::HTTPMethod::GET)([&database](const crow::request& request) {
        const std::optional<UserAccount> user = getAuthenticatedUser(request, database);
        if (!user) {
            return errorResponse(401, "Please log in.");
        }
        const std::optional<UserLocation> location = database.getLocation(user->id);
        if (!location) {
            return errorResponse(404, "No location is saved for this account.");
        }
        return jsonResponse(200, {{"location", locationJson(*location)}});
    });

    // Save (POST /api/location/save) and update (PUT /api/location/update) do the same thing:
    // every account always has one location, so saving replaces it.
    const auto saveLocation = [&database](const crow::request& request) {
        const std::optional<UserAccount> user = getAuthenticatedUser(request, database);
        if (!user) {
            return errorResponse(401, "Please log in.");
        }
        const std::optional<json> body = readJson(request);
        if (!body) {
            return errorResponse(400, "Request body must be a JSON object.");
        }
        std::string locationError;
        const std::optional<UserLocation> location = readLocation(*body, locationError);
        if (!location) {
            return errorResponse(400, locationError);
        }
        if (!database.updateLocation(user->id, *location)) {
            return errorResponse(500, database.getLastError());
        }
        return jsonResponse(200, {{"message", "Your location has been saved."}, {"location", locationJson(*location)}});
    };
    CROW_ROUTE(app, "/api/location/save").methods(crow::HTTPMethod::POST)(saveLocation);
    CROW_ROUTE(app, "/api/location/update").methods(crow::HTTPMethod::PUT)(saveLocation);

    CROW_ROUTE(app, "/api/eligibility-questions").methods(crow::HTTPMethod::GET)([]() {
        return jsonResponse(200, {
            {"questions", eligibilityQuestionsJson()},
            {"note", "This is a self-check. The blood centre will also check your haemoglobin and "
                     "blood pressure before you donate."}
        });
    });

    // ---------------- Receiver: create a request and see the ranked donors ----------------

    CROW_ROUTE(app, "/api/requests").methods(crow::HTTPMethod::POST)([&database](const crow::request& request) {
        const std::optional<UserAccount> user = getAuthenticatedUser(request, database);
        if (!user) {
            return errorResponse(401, "Please log in.");
        }

        const std::optional<json> body = readJson(request);
        const auto hospitalId = body ? intField(*body, "hospital_id") : std::nullopt;
        const auto bloodType = body ? textField(*body, "blood_type", 3) : std::nullopt;
        const auto units = body ? intField(*body, "units_required") : std::nullopt;
        if (!hospitalId || !bloodType || !units) {
            return errorResponse(400, "Hospital, blood group and number of units are required.");
        }
        const std::optional<RadiusChoice> radius = radiusFromBody(*body);
        if (!radius) {
            return errorResponse(400, INVALID_RADIUS);
        }
        // The hospital must be able to take a donor visit now (closed, closing within
        // 30 minutes, outside its hours or permanently closed: refused).
        const std::optional<HospitalRecord> hospital = database.getHospital(*hospitalId);
        if (!hospital || hospital->verificationStatus != "verified") {
            return errorResponse(400, "Choose a hospital from the list.");
        }
        const HospitalStatus hospitalStatus = currentHospitalStatus(*hospital);
        if (!hospitalStatus.acceptsNewDonations) {
            return jsonResponse(409, {{"error", hospitalUnavailableMessage()},
                                      {"hospital_status", hospitalStatus.message}});
        }
        if (!isValidBloodType(*bloodType)) {
            return errorResponse(400, "Choose a valid blood group.");
        }
        if (*units < 1 || *units > MAX_UNITS_PER_REQUEST) {
            return errorResponse(400, "Units required must be between 1 and " +
                                          std::to_string(MAX_UNITS_PER_REQUEST) + ".");
        }

        const std::optional<int> requestId =
            database.createBloodRequest(user->id, *hospitalId, *bloodType, *units);
        if (!requestId) {
            return errorResponse(400, database.getLastError());
        }

        // No donor is contacted yet: the receiver chooses whom to invite from this ranking.
        const RadiusSearchResult search = runRadiusSearch(
            *requestId, *hospitalId, *bloodType, *radius,
            database.loadDonorPool(*requestId, *hospitalId, user->id));

        json response = radiusSearchJson(search);
        response["request_id"] = *requestId;
        response["status"] = RequestStatus::OPEN;
        response["units_required"] = *units;
        response["max_open_invitations"] = MAX_OPEN_INVITATIONS_PER_REQUEST;
        response["invitation_lifetime_minutes"] = INVITATION_LIFETIME_MINUTES;
        return jsonResponse(201, response);
    });

    CROW_ROUTE(app, "/api/my-requests").methods(crow::HTTPMethod::GET)([&database](const crow::request& request) {
        const std::optional<UserAccount> user = getAuthenticatedUser(request, database);
        if (!user) {
            return errorResponse(401, "Please log in.");
        }
        database.expireOldInvitations();
        return jsonResponse(200, {{"requests", database.getReceiverRequests(user->id)}});
    });

    CROW_ROUTE(app, "/api/requests/<int>").methods(crow::HTTPMethod::GET)(
        [&database](const crow::request& request, int requestId) {
            const std::optional<UserAccount> user = getAuthenticatedUser(request, database);
            if (!user) {
                return errorResponse(401, "Please log in.");
            }
            database.expireOldInvitations();
            const json requests = database.getReceiverRequests(user->id, requestId);
            if (requests.empty()) {
                return errorResponse(404, "Blood request was not found.");
            }
            return jsonResponse(200, requests[0]);
        });

    CROW_ROUTE(app, "/api/requests/<int>/candidates").methods(crow::HTTPMethod::GET)(
        [&database](const crow::request& request, int requestId) {
            const std::optional<UserAccount> user = getAuthenticatedUser(request, database);
            if (!user) {
                return errorResponse(401, "Please log in.");
            }
            crow::response error;
            const std::optional<BloodRequestRecord> record = loadOwnedRequest(database, *user, requestId, error);
            if (!record) {
                return error;
            }
            if (record->status != RequestStatus::OPEN) {
                return errorResponse(409, "This request is no longer open.");
            }
            const std::optional<RadiusChoice> radius = radiusFromQuery(request);
            if (!radius) {
                return errorResponse(400, INVALID_RADIUS);
            }

            database.expireOldInvitations();
            const RadiusSearchResult search = runRadiusSearch(
                record->id, record->hospitalId, record->bloodType, *radius,
                database.loadDonorPool(record->id, record->hospitalId, record->receiverId));
            const int openInvitations = database.countInvitations(record->id, InvitationStatus::PENDING);

            json response = radiusSearchJson(search);
            response["request_id"] = record->id;
            response["open_invitations"] = openInvitations;
            response["invitations_left"] = MAX_OPEN_INVITATIONS_PER_REQUEST - openInvitations;
            return jsonResponse(200, response);
        });

    // Body: {"donor_ids": [12, 7, 30]}  or  {"top": 3} to invite the best-ranked donors.
    CROW_ROUTE(app, "/api/requests/<int>/invite").methods(crow::HTTPMethod::POST)(
        [&database](const crow::request& request, int requestId) {
            const std::optional<UserAccount> user = getAuthenticatedUser(request, database);
            if (!user) {
                return errorResponse(401, "Please log in.");
            }
            const std::optional<json> body = readJson(request);
            if (!body) {
                return errorResponse(400, "Request body must be a JSON object.");
            }
            // The radius the receiver is looking at, so only donors shown to them can be invited.
            const std::optional<RadiusChoice> radius = radiusFromBody(*body);
            if (!radius) {
                return errorResponse(400, INVALID_RADIUS);
            }

            Transaction transaction(database);
            if (!transaction.ok() || !database.expireOldInvitations()) {
                return errorResponse(500, database.getLastError());
            }

            crow::response error;
            const std::optional<BloodRequestRecord> record = loadOwnedRequest(database, *user, requestId, error);
            if (!record) {
                return error;
            }
            if (record->status != RequestStatus::OPEN) {
                return errorResponse(409, "This request is no longer open.");
            }

            const int openInvitations = database.countInvitations(record->id, InvitationStatus::PENDING);
            const int slotsLeft = MAX_OPEN_INVITATIONS_PER_REQUEST - openInvitations;
            if (slotsLeft <= 0) {
                return errorResponse(409, "You already have " + std::to_string(MAX_OPEN_INVITATIONS_PER_REQUEST) +
                                              " invitations waiting. Wait for answers or for them to expire.");
            }

            RadiusSearchResult search = runRadiusSearch(
                record->id, record->hospitalId, record->bloodType, *radius,
                database.loadDonorPool(record->id, record->hospitalId, record->receiverId));
            MatchRequest& match = search.match;

            // Decide whom to invite. Either way, only donors from the server's own
            // ranking can be invited, so the browser cannot invite arbitrary users.
            std::deque<Donor> toInvite;
            const auto top = intField(*body, "top");
            const auto idsField = body->find("donor_ids");
            if (top) {
                if (*top < 1 || *top > slotsLeft) {
                    return errorResponse(400, "You can invite between 1 and " + std::to_string(slotsLeft) +
                                                  " more donors right now.");
                }
                for (int count = 0; count < *top; ++count) {
                    const std::optional<Donor> best = match.popHighestRankedDonor();
                    if (!best) {
                        break;
                    }
                    toInvite.push_back(*best);
                }
            } else if (idsField != body->end() && idsField->is_array() && !idsField->empty()) {
                std::unordered_set<int> requested;
                for (const json& id : *idsField) {
                    if (!id.is_number_integer()) {
                        return errorResponse(400, "donor_ids must be a list of donor numbers.");
                    }
                    requested.insert(id.get<int>());
                }
                if (static_cast<int>(requested.size()) > slotsLeft) {
                    return errorResponse(409, "You can invite only " + std::to_string(slotsLeft) +
                                                  " more donors right now (maximum " +
                                                  std::to_string(MAX_OPEN_INVITATIONS_PER_REQUEST) +
                                                  " waiting at a time).");
                }
                for (const Donor& donor : match.getRankedDonors()) {
                    if (requested.erase(donor.getId()) > 0) {
                        toInvite.push_back(donor);
                    }
                }
                if (!requested.empty()) {
                    return errorResponse(400, "Some selected donors cannot be invited. They may have been "
                                              "invited already, become unavailable, or be outside the search area. "
                                              "Refresh the donor list and try again.");
                }
            } else {
                return errorResponse(400, "Send donor_ids (a list of donor numbers) or top (a number).");
            }

            if (toInvite.empty()) {
                return errorResponse(409, "There are no more donors available to invite.");
            }

            json invited = json::array();
            for (const Donor& donor : toInvite) {
                const std::optional<int> invitationId = database.createInvitation(record->id, donor.getId());
                if (!invitationId) {
                    return errorResponse(409, database.getLastError());
                }
                invited.push_back({{"invitation_id", *invitationId}, {"donor_id", donor.getId()},
                                   {"donor_name", donor.getName()}});
            }
            if (!transaction.commit()) {
                return errorResponse(500, database.getLastError());
            }

            return jsonResponse(201, {
                {"invited", invited},
                {"open_invitations", openInvitations + static_cast<int>(invited.size())},
                {"expires_in_minutes", INVITATION_LIFETIME_MINUTES}
            });
        });

    // The receiver confirms the donation took place. This records the donation date
    // (restarting the donor's 90-day interval) and allows rating the donor.
    CROW_ROUTE(app, "/api/requests/<int>/complete").methods(crow::HTTPMethod::POST)(
        [&database](const crow::request& request, int requestId) {
            const std::optional<UserAccount> user = getAuthenticatedUser(request, database);
            if (!user) {
                return errorResponse(401, "Please log in.");
            }

            Transaction transaction(database);
            if (!transaction.ok()) {
                return errorResponse(500, database.getLastError());
            }
            crow::response error;
            const std::optional<BloodRequestRecord> record = loadOwnedRequest(database, *user, requestId, error);
            if (!record) {
                return error;
            }
            if (record->status != RequestStatus::OPEN && record->status != RequestStatus::FULFILLED) {
                return errorResponse(409, "Only an active request can be marked as completed.");
            }
            if (database.countInvitations(record->id, InvitationStatus::ACCEPTED) == 0) {
                return errorResponse(409, "No donor has accepted this request yet.");
            }
            if (database.cancelPendingInvitations(record->id) < 0 || !database.completeRequest(record->id) ||
                !transaction.commit()) {
                return errorResponse(500, database.getLastError());
            }
            return jsonResponse(200, {{"status", RequestStatus::COMPLETED},
                                      {"message", "Donation recorded. You can now rate your donor."}});
        });

    CROW_ROUTE(app, "/api/requests/<int>/cancel").methods(crow::HTTPMethod::POST)(
        [&database](const crow::request& request, int requestId) {
            const std::optional<UserAccount> user = getAuthenticatedUser(request, database);
            if (!user) {
                return errorResponse(401, "Please log in.");
            }

            Transaction transaction(database);
            if (!transaction.ok()) {
                return errorResponse(500, database.getLastError());
            }
            crow::response error;
            const std::optional<BloodRequestRecord> record = loadOwnedRequest(database, *user, requestId, error);
            if (!record) {
                return error;
            }
            if (record->status != RequestStatus::OPEN && record->status != RequestStatus::FULFILLED) {
                return errorResponse(409, "This request is already closed.");
            }
            if (database.cancelPendingInvitations(record->id) < 0 ||
                !database.updateRequestStatus(record->id, RequestStatus::CANCELLED) || !transaction.commit()) {
                return errorResponse(500, database.getLastError());
            }
            return jsonResponse(200, {{"status", RequestStatus::CANCELLED}, {"message", "Request cancelled."}});
        });

    // Body: {"request_id": 5, "donor_id": 12, "stars": 4}
    CROW_ROUTE(app, "/api/ratings").methods(crow::HTTPMethod::POST)([&database](const crow::request& request) {
        const std::optional<UserAccount> user = getAuthenticatedUser(request, database);
        if (!user) {
            return errorResponse(401, "Please log in.");
        }
        const std::optional<json> body = readJson(request);
        const auto requestId = body ? intField(*body, "request_id") : std::nullopt;
        const auto donorId = body ? intField(*body, "donor_id") : std::nullopt;
        const auto stars = body ? intField(*body, "stars") : std::nullopt;
        if (!requestId || !donorId || !stars) {
            return errorResponse(400, "request_id, donor_id and stars (1 to 5) are required.");
        }

        Transaction transaction(database);
        if (!transaction.ok()) {
            return errorResponse(500, database.getLastError());
        }
        if (!database.addRating(*requestId, user->id, *donorId, *stars)) {
            return errorResponse(409, database.getLastError());
        }
        if (!transaction.commit()) {
            return errorResponse(500, database.getLastError());
        }
        return jsonResponse(201, {{"message", "Thank you for rating your donor."}});
    });

    // ---------------- Urgent hospital needs for donors ----------------
    CROW_ROUTE(app, "/api/emergencies/nearby").methods(crow::HTTPMethod::GET)([&database](const crow::request& request) {
        const std::optional<UserAccount> user = getAuthenticatedUser(request, database);
        if (!user) {
            return errorResponse(401, "Please log in.");
        }
        json posts = json::array();
        for (const EmergencyPost& post : urgentPostsFor(database, *user)) {
            posts.push_back(emergencyPostJson(post));
        }
        json profile = user->isDonor ? database.getDonorProfile(user->id) : json(nullptr);
        return jsonResponse(200, {
            {"posts", posts},
            {"radius_km", EMERGENCY_ALERT_RADIUS_KM},
            // Why the list may be empty for this user.
            {"is_donor", user->isDonor},
            {"available", !profile.is_null() && profile["available"].get<bool>()},
            {"can_donate_again_in_days", profile.is_null() ? 0 : profile["can_donate_again_in_days"].get<int>()}
        });
    });

    // ---------------- "Notify me when this hospital reopens" ----------------
    CROW_ROUTE(app, "/api/reopen-alerts").methods(crow::HTTPMethod::GET)([&database](const crow::request& request) {
        const std::optional<UserAccount> user = getAuthenticatedUser(request, database);
        if (!user) {
            return errorResponse(401, "Please log in.");
        }
        json waiting = json::array();
        for (const ReopenAlert& alert : checkReopenAlerts(database, user->id)) {
            if (alert.notifiedAt.empty()) {
                waiting.push_back(alert.hospitalId);
            }
        }
        return jsonResponse(200, {{"hospital_ids", waiting}});
    });

    CROW_ROUTE(app, "/api/hospitals/<int>/reopen-alert").methods(crow::HTTPMethod::POST, crow::HTTPMethod::DELETE)(
        [&database](const crow::request& request, int hospitalId) {
            const std::optional<UserAccount> user = getAuthenticatedUser(request, database);
            if (!user) {
                return errorResponse(401, "Please log in.");
            }
            if (request.method == crow::HTTPMethod::DELETE) {
                database.removeReopenAlert(user->id, hospitalId);
                return jsonResponse(200, {{"message", "You will not be notified."}});
            }
            const std::optional<HospitalRecord> hospital = database.getHospital(hospitalId);
            if (!hospital || hospital->verificationStatus != "verified") {
                return errorResponse(404, "Hospital was not found.");
            }
            if (hospital->availabilityState == "permanently_closed") {
                return errorResponse(409, "This hospital is permanently closed.");
            }
            if (currentHospitalStatus(*hospital).acceptsNewDonations) {
                return errorResponse(409, "This hospital is open now.");
            }
            if (!database.addReopenAlert(user->id, hospitalId)) {
                return errorResponse(500, database.getLastError());
            }
            return jsonResponse(201, {{"message", "We will let you know when " + hospital->name + " reopens."}});
        });

    // ---------------- Administrators: approve new hospitals ----------------
    // GET /api/admin/hospitals?status=pending|verified|rejected|all (default pending)
    CROW_ROUTE(app, "/api/admin/hospitals").methods(crow::HTTPMethod::GET)([&database](const crow::request& request) {
        crow::response error;
        if (!requireAdmin(request, database, error)) {
            return error;
        }
        const char* statusText = request.url_params.get("status");
        const std::string wanted = statusText ? statusText : "pending";
        if (wanted != "pending" && wanted != "verified" && wanted != "rejected" && wanted != "all") {
            return errorResponse(400, "status must be pending, verified, rejected or all.");
        }
        json hospitals = json::array();
        json counts = {{"pending", 0}, {"verified", 0}, {"rejected", 0}};
        std::vector<HospitalRecord> all = database.listHospitals();
        // Newest registrations first.
        std::stable_sort(all.begin(), all.end(), [](const HospitalRecord& a, const HospitalRecord& b) {
            return a.registeredAt > b.registeredAt;
        });
        for (const HospitalRecord& hospital : all) {
            counts[hospital.verificationStatus] = counts[hospital.verificationStatus].get<int>() + 1;
            if (wanted == "all" || hospital.verificationStatus == wanted) {
                hospitals.push_back(hospitalFullJson(hospital));
            }
        }
        return jsonResponse(200, {{"hospitals", hospitals}, {"counts", counts}, {"status", wanted}});
    });

    // Body: {"status": "verified" | "rejected" | "pending", "note": "..."} (a note is required to reject).
    CROW_ROUTE(app, "/api/admin/hospitals/<int>/verification").methods(crow::HTTPMethod::PUT)(
        [&database](const crow::request& request, int hospitalId) {
            crow::response error;
            if (!requireAdmin(request, database, error)) {
                return error;
            }
            const std::optional<json> body = readJson(request);
            const auto status = body ? textField(*body, "status", 10) : std::nullopt;
            const auto note = body ? optionalTextField(*body, "note", 300) : std::nullopt;
            if (!status || (*status != "verified" && *status != "rejected" && *status != "pending")) {
                return errorResponse(400, "status must be verified, rejected or pending.");
            }
            if (!note) {
                return errorResponse(400, "The note can be at most 300 characters.");
            }
            if (*status == "rejected" && note->size() < 5) {
                return errorResponse(400, "Tell the hospital why it was not approved (at least 5 characters).");
            }
            if (!database.setHospitalVerification(hospitalId, *status, *status == "verified" ? "" : *note)) {
                return errorResponse(404, database.getLastError());
            }
            return jsonResponse(200, {{"message", *status == "verified" ? "Hospital approved. It now appears in search."
                                                  : *status == "rejected" ? "Hospital rejected." : "Hospital set back to pending."},
                                      {"hospital", hospitalFullJson(*database.getHospital(hospitalId))}});
        });

    // ---------------- Donor: see invitations and answer them ----------------

    CROW_ROUTE(app, "/api/donor/invitations").methods(crow::HTTPMethod::GET)([&database](const crow::request& request) {
        const std::optional<UserAccount> user = getAuthenticatedUser(request, database);
        if (!user) {
            return errorResponse(401, "Please log in.");
        }
        if (!user->isDonor) {
            return errorResponse(403, "Set up your donor profile first.");
        }
        database.expireOldInvitations();
        return jsonResponse(200, {{"invitations", database.getDonorInvitations(user->id)}});
    });

    // Body: {"available": true/false}. An unavailable donor is not shown to receivers,
    // and turning availability off also withdraws from invitations still waiting.
    CROW_ROUTE(app, "/api/donor/availability").methods(crow::HTTPMethod::POST)([&database](const crow::request& request) {
        const std::optional<UserAccount> user = getAuthenticatedUser(request, database);
        if (!user) {
            return errorResponse(401, "Please log in.");
        }
        if (!user->isDonor) {
            return errorResponse(403, "Set up your donor profile first.");
        }
        const std::optional<json> body = readJson(request);
        const auto available = body ? boolField(*body, "available") : std::nullopt;
        if (!available) {
            return errorResponse(400, "available (true or false) is required.");
        }

        Transaction transaction(database);
        if (!transaction.ok() || !database.setDonorAvailability(user->id, *available)) {
            return errorResponse(500, database.getLastError());
        }
        if (!*available && database.cancelDonorPendingInvitations(user->id) < 0) {
            return errorResponse(500, database.getLastError());
        }
        if (!transaction.commit()) {
            return errorResponse(500, database.getLastError());
        }
        return jsonResponse(200, {
            {"available", *available},
            {"message", *available ? "You are now visible to people who need blood."
                                   : "You are now hidden from new requests."}
        });
    });

    // Body: {"accepted": false}
    //   or  {"accepted": true, "answers": {"age": true, "weight": true, ...}}
    // Accepting requires passing the eligibility questionnaire. As soon as one donor
    // accepts, every other invitation still waiting for this request is cancelled.
    CROW_ROUTE(app, "/api/invitations/<int>/respond").methods(crow::HTTPMethod::POST)(
        [&database](const crow::request& request, int invitationId) {
            const std::optional<UserAccount> user = getAuthenticatedUser(request, database);
            if (!user) {
                return errorResponse(401, "Please log in.");
            }
            if (!user->isDonor) {
                return errorResponse(403, "Set up your donor profile first.");
            }

            const std::optional<json> body = readJson(request);
            const auto accepted = body ? boolField(*body, "accepted") : std::nullopt;
            if (!accepted) {
                return errorResponse(400, "accepted (true or false) is required.");
            }

            // Check the questionnaire before touching the database.
            if (*accepted) {
                const auto answers = readAnswers(*body);
                if (!answers) {
                    return errorResponse(400, "Please answer the eligibility questions before accepting.");
                }
                const EligibilityResult result = evaluateEligibility(*answers);
                if (!result.missingQuestionId.empty()) {
                    return errorResponse(400, "Please answer every eligibility question (missing: " +
                                                  result.missingQuestionId + ").");
                }
                if (!result.eligible) {
                    json reasons = json::array();
                    for (std::size_t index = 0; index < result.reasonCount; ++index) {
                        reasons.push_back(result.reasons[index]);
                    }
                    return jsonResponse(400, {
                        {"error", "Based on your answers, you cannot donate right now. "
                                  "Please decline this request so the receiver can invite someone else."},
                        {"reasons", reasons}
                    });
                }
            }

            Transaction transaction(database);
            if (!transaction.ok() || !database.expireOldInvitations()) {
                return errorResponse(500, database.getLastError());
            }

            const std::optional<InvitationRecord> invitation = database.getInvitation(invitationId);
            // "Not found" for someone else's invitation too.
            if (!invitation || invitation->donorId != user->id) {
                return errorResponse(404, "Invitation was not found.");
            }
            if (invitation->status != InvitationStatus::PENDING) {
                return errorResponse(409, invitationClosedMessage(invitation->status));
            }
            const std::optional<BloodRequestRecord> record = database.getBloodRequest(invitation->requestId);
            if (!record || record->status != RequestStatus::OPEN) {
                return errorResponse(409, invitationClosedMessage(InvitationStatus::CANCELLED));
            }

            if (*accepted && database.hasActiveCommitment(user->id)) {
                return errorResponse(409, "You have already accepted another request. Please complete "
                                          "that donation first.");
            }

            if (!database.respondToInvitation(invitation->id, *accepted)) {
                return errorResponse(409, database.getLastError());
            }

            if (!*accepted) {
                if (!transaction.commit()) {
                    return errorResponse(500, database.getLastError());
                }
                return jsonResponse(200, {{"status", InvitationStatus::REJECTED},
                                          {"message", "You declined this request. Thank you for letting us know."}});
            }

            // One donor accepted: close every other waiting invitation straight away,
            // so those donors are not left waiting.
            // The donor is now committed, so their invitations from other receivers close too.
            if (database.cancelPendingInvitations(record->id) < 0 ||
                database.cancelDonorPendingInvitations(user->id) < 0) {
                return errorResponse(500, database.getLastError());
            }
            const int unitsAccepted = database.countInvitations(record->id, InvitationStatus::ACCEPTED);
            const std::string requestStatus =
                unitsAccepted >= record->unitsRequired ? RequestStatus::FULFILLED : RequestStatus::OPEN;
            if (!database.updateRequestStatus(record->id, requestStatus) || !transaction.commit()) {
                return errorResponse(500, database.getLastError());
            }

            const json requests = database.getReceiverRequests(record->receiverId, record->id);
            json response = {
                {"status", InvitationStatus::ACCEPTED},
                {"request_status", requestStatus},
                {"message", "Thank you for accepting. Please go to the hospital as soon as you can. "
                            "The receiver can now see your phone number."}
            };
            if (!requests.empty()) {
                response["hospital_name"] = requests[0]["hospital_name"];
                response["hospital_address"] = requests[0]["hospital_address"];
            }
            return jsonResponse(200, response);
        });
}
