#include "Routes.h"

#include "PasswordHasher.h"
#include "Validation.h"
#include "../db/Database.h"
#include "../engine/BloodType.h"
#include "../engine/Eligibility.h"
#include "../engine/MatchRequest.h"

#include <nlohmann/json.hpp>

#include <cmath>
#include <fstream>
#include <unordered_map>
#include <unordered_set>
#include <iterator>
#include <limits>
#include <optional>
#include <string>

namespace {
using nlohmann::json;

void addSecurityHeaders(crow::response& response) {
    response.set_header("X-Content-Type-Options", "nosniff");
    response.set_header("Referrer-Policy", "no-referrer");
}

crow::response jsonResponse(int status, const json& body) {
    crow::response response(status, body.dump());
    response.set_header("Content-Type", "application/json");
    addSecurityHeaders(response);
    return response;
}

crow::response errorResponse(int status, const std::string& message) {
    return jsonResponse(status, {{"error", message}});
}

crow::response staticFileResponse(const std::string& path, const char* contentType) {
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        return crow::response(404, "File not found.");
    }

    const std::string content((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    crow::response response(200, content);
    response.set_header("Content-Type", contentType);
    // Always load the latest version of pages and scripts.
    response.set_header("Cache-Control", "no-store");
    addSecurityHeaders(response);
    return response;
}

// The body must be a JSON object; anything else is treated as invalid.
std::optional<json> readJson(const crow::request& request) {
    try {
        json body = json::parse(request.body);
        if (!body.is_object()) {
            return std::nullopt;
        }
        return body;
    } catch (const json::exception&) {
        return std::nullopt;
    }
}

// ---- Typed field readers: each returns nullopt when the field is missing or the wrong type,
// ---- so a bad request becomes a 400 instead of an exception and a 500.

std::optional<std::string> textField(const json& body, const char* name, std::size_t maxLength) {
    const auto field = body.find(name);
    if (field == body.end() || !field->is_string()) {
        return std::nullopt;
    }
    const std::string value = Validation::trim(field->get<std::string>());
    if (value.empty() || value.size() > maxLength) {
        return std::nullopt;
    }
    return value;
}

// Passwords are not trimmed: spaces are allowed and count.
std::optional<std::string> passwordField(const json& body, const char* name) {
    const auto field = body.find(name);
    if (field == body.end() || !field->is_string()) {
        return std::nullopt;
    }
    return field->get<std::string>();
}

std::optional<int> intField(const json& body, const char* name) {
    const auto field = body.find(name);
    if (field == body.end() || !field->is_number_integer()) {
        return std::nullopt;
    }
    const long long value = field->get<long long>();
    if (value < std::numeric_limits<int>::min() || value > std::numeric_limits<int>::max()) {
        return std::nullopt;
    }
    return static_cast<int>(value);
}

std::optional<double> numberField(const json& body, const char* name) {
    const auto field = body.find(name);
    if (field == body.end() || !field->is_number()) {
        return std::nullopt;
    }
    return field->get<double>();
}

std::optional<bool> boolField(const json& body, const char* name) {
    const auto field = body.find(name);
    if (field == body.end() || !field->is_boolean()) {
        return std::nullopt;
    }
    return field->get<bool>();
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
        const auto area = textField(*body, "area", 80);
        const auto latitude = numberField(*body, "latitude");
        const auto longitude = numberField(*body, "longitude");
        if (!name || !rawEmail || !password || !question || !answer) {
            return errorResponse(400, "Name, email, password, security question and answer are required.");
        }
        if (!bloodType || !isValidBloodType(*bloodType)) {
            return errorResponse(400, "Choose your blood group.");
        }
        if (!area || !latitude || !longitude || !Validation::isValidLatitude(*latitude) ||
            !Validation::isValidLongitude(*longitude)) {
            return errorResponse(400, "Choose your area or use your current location.");
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
                                                                 *bloodType, *area, *latitude, *longitude);
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
    CROW_ROUTE(app, "/api/notifications").methods(crow::HTTPMethod::GET)([&database](const crow::request& request) {
        const std::optional<UserAccount> user = getAuthenticatedUser(request, database);
        if (!user) {
            return errorResponse(401, "Please log in.");
        }
        database.expireOldInvitations();
        return jsonResponse(200, database.getNotifications(user->id));
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

    // Profile page. Body: {"name", "blood_type", "area", "latitude", "longitude", "phone" (donors only)}
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
        const auto area = textField(*body, "area", 80);
        const auto latitude = numberField(*body, "latitude");
        const auto longitude = numberField(*body, "longitude");
        if (!name) {
            return errorResponse(400, "Enter your name.");
        }
        if (!bloodType || !isValidBloodType(*bloodType)) {
            return errorResponse(400, "Choose your blood group.");
        }
        if (!area || !latitude || !longitude || !Validation::isValidLatitude(*latitude) ||
            !Validation::isValidLongitude(*longitude)) {
            return errorResponse(400, "Choose your area or use your current location.");
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
            !database.updateProfile(user->id, *name, *bloodType, *area, *latitude, *longitude) ||
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

    CROW_ROUTE(app, "/api/hospitals").methods(crow::HTTPMethod::GET)([&database]() {
        return jsonResponse(200, {{"hospitals", database.getHospitals()}});
    });

    CROW_ROUTE(app, "/api/me").methods(crow::HTTPMethod::GET)([&database](const crow::request& request) {
        const std::optional<UserAccount> user = getAuthenticatedUser(request, database);
        if (!user) {
            return errorResponse(401, "Please log in.");
        }
        json response = {{"user_id", user->id}, {"name", user->name}, {"email", user->email},
                         {"blood_type", user->bloodType}, {"area", user->area},
                         {"latitude", user->latitude}, {"longitude", user->longitude},
                         {"is_donor", user->isDonor}, {"donor_profile", nullptr}};
        if (user->isDonor) {
            response["donor_profile"] = database.getDonorProfile(user->id);
        }
        return jsonResponse(200, response);
    });

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
        MatchRequest match(*requestId, *hospitalId, *bloodType);
        database.loadCandidatesForRequest(match, user->id);

        return jsonResponse(201, {
            {"request_id", *requestId},
            {"status", RequestStatus::OPEN},
            {"units_required", *units},
            {"search_radius_km", match.getSearchRadiusKm()},
            {"max_open_invitations", MAX_OPEN_INVITATIONS_PER_REQUEST},
            {"invitation_lifetime_minutes", INVITATION_LIFETIME_MINUTES},
            {"candidates", candidatesJson(match)}
        });
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

            database.expireOldInvitations();
            MatchRequest match(record->id, record->hospitalId, record->bloodType);
            database.loadCandidatesForRequest(match, record->receiverId);
            const int openInvitations = database.countInvitations(record->id, InvitationStatus::PENDING);

            return jsonResponse(200, {
                {"request_id", record->id},
                {"open_invitations", openInvitations},
                {"invitations_left", MAX_OPEN_INVITATIONS_PER_REQUEST - openInvitations},
                {"candidates", candidatesJson(match)}
            });
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

            MatchRequest match(record->id, record->hospitalId, record->bloodType);
            database.loadCandidatesForRequest(match, record->receiverId);

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
