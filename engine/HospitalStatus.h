#ifndef HOSPITAL_STATUS_H
#define HOSPITAL_STATUS_H

#include <array>
#include <optional>
#include <string>

// Works out whether a hospital can take a new donor visit right now.
// Everything here is plain C++ with the current time passed in, so it can be unit-tested
// and is always decided on the server (the browser only displays the result).

// A hospital that closes within this many minutes is treated as closed for new visits.
constexpr int CLOSED_BEFORE_CLOSING_MINUTES = 30;
// From this many minutes before its regular closing time, an open hospital shows "Closing soon".
constexpr int REGULAR_CLOSING_NOTICE_MINUTES = 60;
// "Closing soon" set by the hospital: 1 minute to 12 hours ahead.
constexpr int MIN_CLOSING_SOON_MINUTES = 1;
constexpr int MAX_CLOSING_SOON_MINUTES = 720;

constexpr int MINUTES_PER_DAY = 24 * 60;
constexpr int MINUTES_PER_WEEK = 7 * MINUTES_PER_DAY;

// What the hospital has set in its dashboard.
enum class HospitalState { Open, ClosingSoon, TemporarilyClosed, PermanentlyClosed };

// Regular hours. Times are minutes after midnight (local time). openMinute == closeMinute
// means open all day; closeMinute < openMinute means the shift runs past midnight.
// workingDays[0] is Monday ... workingDays[6] is Sunday (the day a shift starts).
struct OpeningHours {
    int openMinute = 0;
    int closeMinute = 0;
    std::array<bool, 7> workingDays{{true, true, true, true, true, true, true}};
};

// The current local time: weekday (0 = Monday) and minutes after midnight.
struct LocalTime {
    int weekday = 0;
    int minuteOfDay = 0;
};

struct HospitalStatus {
    std::string code;        // "open", "closing_soon", "closed" or "permanently_closed"
    std::string badge;       // "Open", "Closing soon", "Closed" or "Permanently Closed"
    std::string message;     // one sentence for the user
    int minutesUntilClose;   // -1 when the hospital is not about to close (or is closed)
    bool acceptsNewDonations;
};

// secondsUntilManualClose: for HospitalState::ClosingSoon, the seconds left until the
// closing time the hospital set (0 or less means that time has passed).
HospitalStatus evaluateHospitalStatus(HospitalState state, std::optional<long long> secondsUntilManualClose,
                                      const OpeningHours& hours, LocalTime now);

// Minutes until the regular hours end, or nullopt when the hospital is outside its hours now.
// Returns MINUTES_PER_WEEK or more when it never closes (open all day, every day).
std::optional<int> minutesUntilRegularClose(const OpeningHours& hours, LocalTime now);
// Minutes until the regular hours next begin (0 when open now), or nullopt with no working days.
std::optional<int> minutesUntilRegularOpen(const OpeningHours& hours, LocalTime now);

// "09:30" -> 570. Returns nullopt for anything that is not a valid 24-hour HH:MM time.
std::optional<int> parseClockTime(const std::string& text);
std::string formatClockTime(int minuteOfDay);

bool isValidClosingSoonMinutes(int minutes);

// "open", "closing_soon", "temporarily_closed", "permanently_closed" <-> HospitalState.
std::optional<HospitalState> parseHospitalState(const std::string& text);
std::string hospitalStateName(HospitalState state);

#endif
