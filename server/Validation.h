#ifndef VALIDATION_H
#define VALIDATION_H

#include <cctype>
#include <cmath>
#include <ctime>
#include <regex>
#include <string>

// Input checks shared by the API routes.
namespace Validation {

inline std::string trim(const std::string& value) {
    const auto first = value.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) {
        return "";
    }
    const auto last = value.find_last_not_of(" \t\r\n");
    return value.substr(first, last - first + 1);
}

inline std::string toLower(std::string value) {
    for (char& character : value) {
        character = static_cast<char>(std::tolower(static_cast<unsigned char>(character)));
    }
    return value;
}

// Emails are stored trimmed and lowercase so "A@x.com" and "a@x.com" are one account.
inline std::string normalizeEmail(const std::string& email) {
    return toLower(trim(email));
}

inline bool isValidEmail(const std::string& email) {
    static const std::regex pattern(R"(^[a-z0-9._%+\-]+@[a-z0-9\-]+(\.[a-z0-9\-]+)+$)");
    return email.size() <= 254 && std::regex_match(email, pattern);
}

inline bool isValidPassword(const std::string& password) {
    return password.size() >= 8 && password.size() <= 128;
}

// Indian mobile number: exactly 10 digits starting with 6, 7, 8 or 9.
inline bool isValidPhone(const std::string& phone) {
    static const std::regex pattern(R"(^[6-9][0-9]{9}$)");
    return std::regex_match(phone, pattern);
}

inline bool isValidLatitude(double value) {
    return std::isfinite(value) && value >= -90.0 && value <= 90.0;
}

inline bool isValidLongitude(double value) {
    return std::isfinite(value) && value >= -180.0 && value <= 180.0;
}

// Security answers are compared case- and spacing-insensitively:
// "  New  Delhi " and "new delhi" match.
inline std::string normalizeAnswer(const std::string& answer) {
    std::string result;
    bool previousWasSpace = false;
    for (char character : trim(answer)) {
        if (std::isspace(static_cast<unsigned char>(character))) {
            if (!previousWasSpace) {
                result += ' ';
            }
            previousWasSpace = true;
        } else {
            result += static_cast<char>(std::tolower(static_cast<unsigned char>(character)));
            previousWasSpace = false;
        }
    }
    return result;
}

inline std::string todayIso() {
    const std::time_t now = std::time(nullptr);
    char buffer[11] = {};
    std::strftime(buffer, sizeof(buffer), "%Y-%m-%d", std::localtime(&now));
    return buffer;
}

// A real calendar date in YYYY-MM-DD form, not after `today`.
inline bool isValidPastDate(const std::string& date, const std::string& today = todayIso()) {
    static const std::regex pattern(R"(^(\d{4})-(\d{2})-(\d{2})$)");
    std::smatch parts;
    if (!std::regex_match(date, parts, pattern)) {
        return false;
    }

    const int year = std::stoi(parts[1].str());
    const int month = std::stoi(parts[2].str());
    const int day = std::stoi(parts[3].str());
    if (year < 1900 || month < 1 || month > 12 || day < 1) {
        return false;
    }

    static const int daysInMonth[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    const bool leapYear = (year % 4 == 0 && year % 100 != 0) || year % 400 == 0;
    const int maxDay = (month == 2 && leapYear) ? 29 : daysInMonth[month - 1];

    // ISO dates compare correctly as plain strings.
    return day <= maxDay && date <= today;
}

}

#endif
