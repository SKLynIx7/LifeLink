#ifndef HTTP_HELPERS_H
#define HTTP_HELPERS_H

// Helpers shared by the route files: building JSON responses and reading request bodies
// safely. Each field reader returns nullopt for a missing or wrong-typed value, so a bad
// request becomes a 400 error instead of an exception.

#include "Validation.h"

#include "crow.h"
#include <nlohmann/json.hpp>

#include <fstream>
#include <iterator>
#include <limits>
#include <optional>
#include <string>

namespace http {
using nlohmann::json;

inline void addSecurityHeaders(crow::response& response) {
    response.set_header("X-Content-Type-Options", "nosniff");
    response.set_header("Referrer-Policy", "no-referrer");
}

inline crow::response jsonResponse(int status, const json& body) {
    crow::response response(status, body.dump());
    response.set_header("Content-Type", "application/json");
    addSecurityHeaders(response);
    return response;
}

inline crow::response errorResponse(int status, const std::string& message) {
    return jsonResponse(status, {{"error", message}});
}

inline crow::response staticFileResponse(const std::string& path, const char* contentType) {
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
inline std::optional<json> readJson(const crow::request& request) {
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

inline std::optional<std::string> textField(const json& body, const char* name, std::size_t maxLength) {
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
inline std::optional<std::string> passwordField(const json& body, const char* name) {
    const auto field = body.find(name);
    if (field == body.end() || !field->is_string()) {
        return std::nullopt;
    }
    return field->get<std::string>();
}

inline std::optional<int> intField(const json& body, const char* name) {
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

inline std::optional<double> numberField(const json& body, const char* name) {
    const auto field = body.find(name);
    if (field == body.end() || !field->is_number()) {
        return std::nullopt;
    }
    return field->get<double>();
}

inline std::optional<bool> boolField(const json& body, const char* name) {
    const auto field = body.find(name);
    if (field == body.end() || !field->is_boolean()) {
        return std::nullopt;
    }
    return field->get<bool>();
}

// Optional text: a missing or null field gives "", a wrong type or an over-long value gives nullopt.
inline std::optional<std::string> optionalTextField(const json& body, const char* name, std::size_t maxLength) {
    const auto field = body.find(name);
    if (field == body.end() || field->is_null()) {
        return std::string();
    }
    if (!field->is_string()) {
        return std::nullopt;
    }
    const std::string value = Validation::trim(field->get<std::string>());
    if (value.size() > maxLength) {
        return std::nullopt;
    }
    return value;
}

}  // namespace http

#endif
