#include "Routes.h"

#include "PasswordHasher.h"
#include "../db/Database.h"
#include "../engine/MatchRequest.h"

#include <nlohmann/json.hpp>

#include <optional>
#include <string>

namespace {
crow::response jsonResponse(int status, const nlohmann::json& body) {
    crow::response response(status, body.dump());
    response.set_header("Content-Type", "application/json");
    return response;
}

std::optional<nlohmann::json> readJson(const crow::request& request) {
    try {
        return nlohmann::json::parse(request.body);
    } catch (const nlohmann::json::exception&) {
        return std::nullopt;
    }
}

bool hasTextField(const nlohmann::json& body, const char* name) {
    return body.contains(name) && body[name].is_string() && !body[name].get<std::string>().empty();
}

std::optional<UserAccount> getAuthenticatedUser(const crow::request& request, Database& database) {
    const std::string token = request.get_header_value("X-Session-Token");
    if (token.empty()) {
        return std::nullopt;
    }

    const std::optional<int> userId = database.getSessionUserId(token);
    return userId.has_value() ? database.findUserById(*userId) : std::nullopt;
}

nlohmann::json donorJson(const Donor& donor) {
    return {
        {"id", donor.getId()},
        {"name", donor.getName()},
        {"phone", donor.getPhone()},
        {"blood_type", donor.getBloodType()},
        {"distance_km", donor.getDistanceKm()},
        {"reliability_score", donor.getReliabilityScore()}
    };
}
}

void registerRoutes(crow::SimpleApp& app, Database& database) {
    CROW_ROUTE(app, "/ping")([]() {
        return jsonResponse(200, {{"status", "ok"}});
    });

    CROW_ROUTE(app, "/api/register").methods(crow::HTTPMethod::POST)([&database](const crow::request& request) {
        const std::optional<nlohmann::json> body = readJson(request);
        if (!body.has_value() || !hasTextField(*body, "name") || !hasTextField(*body, "email") ||
            !hasTextField(*body, "password") || !hasTextField(*body, "role") ||
            !hasTextField(*body, "security_question") || !hasTextField(*body, "security_answer")) {
            return jsonResponse(400, {{"error", "Missing registration fields."}});
        }

        const std::string role = (*body)["role"].get<std::string>();
        if (role != "donor" && role != "receiver") {
            return jsonResponse(400, {{"error", "Role must be donor or receiver."}});
        }
        if (role == "donor" && (!hasTextField(*body, "phone") || !hasTextField(*body, "blood_type") ||
                                 !body->contains("latitude") || !body->contains("longitude"))) {
            return jsonResponse(400, {{"error", "Donor phone, blood type and location are required."}});
        }

        const std::string passwordHash = PasswordHasher::hashPassword((*body)["password"].get<std::string>());
        if (passwordHash.empty()) {
            return jsonResponse(500, {{"error", "Could not secure the password."}});
        }

        const std::optional<int> userId = database.registerUser(
            (*body)["name"].get<std::string>(), (*body)["email"].get<std::string>(), passwordHash, role,
            (*body)["security_question"].get<std::string>(), (*body)["security_answer"].get<std::string>());
        if (!userId.has_value()) {
            return jsonResponse(400, {{"error", database.getLastError()}});
        }

        if (role == "donor" && !database.createDonorProfile(
                *userId, (*body)["phone"].get<std::string>(), (*body)["blood_type"].get<std::string>(),
                (*body)["latitude"].get<double>(), (*body)["longitude"].get<double>())) {
            return jsonResponse(400, {{"error", database.getLastError()}});
        }

        return jsonResponse(201, {{"message", "Registration successful."}, {"user_id", *userId}});
    });

    CROW_ROUTE(app, "/api/login").methods(crow::HTTPMethod::POST)([&database](const crow::request& request) {
        const std::optional<nlohmann::json> body = readJson(request);
        if (!body.has_value() || !hasTextField(*body, "email") || !hasTextField(*body, "password")) {
            return jsonResponse(400, {{"error", "Email and password are required."}});
        }

        const std::string email = (*body)["email"].get<std::string>();
        const std::optional<std::string> storedHash = database.getPasswordHash(email);
        const std::optional<UserAccount> user = database.findUserByEmail(email);
        if (!storedHash.has_value() || !user.has_value() ||
            !PasswordHasher::verifyPassword((*body)["password"].get<std::string>(), *storedHash)) {
            return jsonResponse(401, {{"error", "Invalid email or password."}});
        }

        const std::string token = PasswordHasher::createSessionToken();
        if (token.empty() || !database.createSession(user->id, token)) {
            return jsonResponse(500, {{"error", "Could not create a session."}});
        }

        return jsonResponse(200, {{"session_token", token}, {"role", user->role}, {"user_id", user->id}});
    });

    CROW_ROUTE(app, "/api/forgot-password").methods(crow::HTTPMethod::POST)([&database](const crow::request& request) {
        const std::optional<nlohmann::json> body = readJson(request);
        if (!body.has_value() || !hasTextField(*body, "email") || !hasTextField(*body, "security_answer") ||
            !hasTextField(*body, "new_password")) {
            return jsonResponse(400, {{"error", "Email, security answer and new password are required."}});
        }

        const std::string email = (*body)["email"].get<std::string>();
        if (!database.verifySecurityAnswer(email, (*body)["security_answer"].get<std::string>())) {
            return jsonResponse(401, {{"error", "Security answer did not match."}});
        }

        const std::string passwordHash = PasswordHasher::hashPassword((*body)["new_password"].get<std::string>());
        if (passwordHash.empty() || !database.updatePassword(email, passwordHash)) {
            return jsonResponse(500, {{"error", "Could not update password."}});
        }

        return jsonResponse(200, {{"message", "Password updated."}});
    });

    CROW_ROUTE(app, "/api/hospitals").methods(crow::HTTPMethod::GET)([&database]() {
        return jsonResponse(200, {{"hospitals", database.getHospitals()}});
    });

    CROW_ROUTE(app, "/api/requests").methods(crow::HTTPMethod::POST)([&database](const crow::request& request) {
        const std::optional<UserAccount> user = getAuthenticatedUser(request, database);
        const std::optional<nlohmann::json> body = readJson(request);
        if (!user.has_value() || user->role != "receiver") {
            return jsonResponse(401, {{"error", "Receiver login is required."}});
        }
        if (!body.has_value() || !hasTextField(*body, "blood_type") || !body->contains("hospital_id") ||
            !body->contains("units_required")) {
            return jsonResponse(400, {{"error", "Hospital, blood type and units are required."}});
        }

        const std::optional<int> requestId = database.createBloodRequest(
            user->id, (*body)["hospital_id"].get<int>(), (*body)["blood_type"].get<std::string>(),
            (*body)["units_required"].get<int>());
        if (!requestId.has_value()) {
            return jsonResponse(400, {{"error", database.getLastError()}});
        }

        MatchRequest match(*requestId, (*body)["hospital_id"].get<int>(), user->name,
                           (*body)["blood_type"].get<std::string>(), (*body)["units_required"].get<int>());
        database.loadCandidatesForRequest(match);
        const std::optional<Donor> topDonor = match.getHighestRankedDonor();

        nlohmann::json response = {
            {"request_id", *requestId},
            {"eligible_candidates", match.getEligibleCandidateCount()},
            {"status", match.getStatus()},
            {"nearby_lower_rated_donors", database.getNearbyLowerRatedDonors(match)}
        };
        if (topDonor.has_value()) {
            response["first_donor_to_contact"] = donorJson(*topDonor);
        }
        return jsonResponse(201, response);
    });

    CROW_ROUTE(app, "/api/donor-response").methods(crow::HTTPMethod::POST)([&database](const crow::request& request) {
        const std::optional<UserAccount> user = getAuthenticatedUser(request, database);
        const std::optional<nlohmann::json> body = readJson(request);
        if (!user.has_value() || user->role != "donor") {
            return jsonResponse(401, {{"error", "Donor login is required."}});
        }
        if (!body.has_value() || !body->contains("request_id") || !body->contains("accepted")) {
            return jsonResponse(400, {{"error", "Request ID and response are required."}});
        }

        const std::optional<BloodRequestRecord> savedRequest = database.getBloodRequest((*body)["request_id"].get<int>());
        if (!savedRequest.has_value()) {
            return jsonResponse(404, {{"error", "Blood request was not found."}});
        }

        MatchRequest match(savedRequest->id, savedRequest->hospitalId, "receiver",
                           savedRequest->bloodType, savedRequest->unitsRequired);
        database.loadCandidatesForRequest(match);
        const std::optional<Donor> topDonor = match.getHighestRankedDonor();
        if (!topDonor.has_value()) {
            return jsonResponse(409, {{"error", "No donor is waiting for a response."}});
        }
        if (topDonor->getId() != user->id) {
            return jsonResponse(409, {{"error", "This donor is not currently first in the ranking."}});
        }

        const bool accepted = (*body)["accepted"].get<bool>();
        match.recordResponse(user->id, accepted);
        if (!database.saveDonorResponse(savedRequest->id, user->id, accepted)) {
            return jsonResponse(500, {{"error", database.getLastError()}});
        }

        nlohmann::json response = {{"status", match.getStatus()}};
        const std::optional<Donor> nextDonor = match.getHighestRankedDonor();
        if (nextDonor.has_value()) {
            response["next_donor_to_contact"] = donorJson(*nextDonor);
        }
        return jsonResponse(200, response);
    });
}
