#include "HospitalStatus.h"

#include <algorithm>
#include <vector>

namespace {
struct Interval {
    int start;
    int end;  // exclusive
};

// The open periods of two consecutive weeks, in minutes from Monday 00:00 of the first week,
// merged so back-to-back periods (e.g. 24-hour days, or an overnight shift into an all-day
// shift) count as one. Two weeks are used so periods that cross Sunday midnight are seen.
std::vector<Interval> openPeriods(const OpeningHours& hours) {
    std::vector<Interval> periods;
    for (int week = 0; week < 2; ++week) {
        for (int day = 0; day < 7; ++day) {
            if (!hours.workingDays[day]) {
                continue;
            }
            const int dayStart = week * MINUTES_PER_WEEK + day * MINUTES_PER_DAY;
            if (hours.openMinute == hours.closeMinute) {
                periods.push_back({dayStart, dayStart + MINUTES_PER_DAY});
            } else if (hours.openMinute < hours.closeMinute) {
                periods.push_back({dayStart + hours.openMinute, dayStart + hours.closeMinute});
            } else {
                periods.push_back({dayStart + hours.openMinute, dayStart + MINUTES_PER_DAY + hours.closeMinute});
            }
        }
    }
    std::sort(periods.begin(), periods.end(), [](const Interval& a, const Interval& b) { return a.start < b.start; });

    std::vector<Interval> merged;
    for (const Interval& period : periods) {
        if (!merged.empty() && period.start <= merged.back().end) {
            merged.back().end = std::max(merged.back().end, period.end);
        } else {
            merged.push_back(period);
        }
    }
    return merged;
}

int minuteOfWeek(LocalTime now) {
    return now.weekday * MINUTES_PER_DAY + now.minuteOfDay;
}

const char* DAY_NAMES[] = {"Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday", "Sunday"};

std::string whenItOpens(const OpeningHours& hours, LocalTime now) {
    const std::optional<int> wait = minutesUntilRegularOpen(hours, now);
    if (!wait) {
        return "This hospital is closed now.";
    }
    const int opensAt = minuteOfWeek(now) + *wait;
    const int day = (opensAt / MINUTES_PER_DAY) % 7;
    const int daysAhead = opensAt / MINUTES_PER_DAY - now.weekday;
    const std::string when = daysAhead == 0 ? "today" : daysAhead == 1 ? "tomorrow" : std::string("on ") + DAY_NAMES[day];
    return "This hospital is closed now. It opens " + when + " at " + formatClockTime(opensAt % MINUTES_PER_DAY) + ".";
}

HospitalStatus closedStatus(const std::string& message) {
    return {"closed", "Closed", message, -1, false};
}
}  // namespace

std::optional<int> minutesUntilRegularClose(const OpeningHours& hours, LocalTime now) {
    const int t = minuteOfWeek(now);
    for (const Interval& period : openPeriods(hours)) {
        // A period from last Sunday night is found by looking one week later.
        for (int at : {t, t + MINUTES_PER_WEEK}) {
            if (period.start <= at && at < period.end) {
                return period.end - at;
            }
        }
    }
    return std::nullopt;
}

std::optional<int> minutesUntilRegularOpen(const OpeningHours& hours, LocalTime now) {
    if (minutesUntilRegularClose(hours, now)) {
        return 0;
    }
    const int t = minuteOfWeek(now);
    for (const Interval& period : openPeriods(hours)) {  // sorted by start
        if (period.start > t) {
            return period.start - t;
        }
    }
    return std::nullopt;
}

HospitalStatus evaluateHospitalStatus(HospitalState state, std::optional<long long> secondsUntilManualClose,
                                      const OpeningHours& hours, LocalTime now) {
    if (state == HospitalState::PermanentlyClosed) {
        return {"permanently_closed", "Permanently Closed", "This hospital is permanently closed.", -1, false};
    }
    if (state == HospitalState::TemporarilyClosed) {
        return closedStatus("This hospital is temporarily closed.");
    }

    // Outside regular hours the hospital is closed, whatever else is set.
    const std::optional<int> regularMinutes = minutesUntilRegularClose(hours, now);
    if (!regularMinutes) {
        return closedStatus(whenItOpens(hours, now));
    }

    // The next closing is the earlier of the regular closing time and the one the hospital set.
    std::optional<int> minutesLeft;
    if (*regularMinutes < MINUTES_PER_WEEK) {
        minutesLeft = *regularMinutes;
    }
    const bool manualClosing = state == HospitalState::ClosingSoon;
    if (manualClosing) {
        if (!secondsUntilManualClose || *secondsUntilManualClose <= 0) {
            // The closing time the hospital set has passed: closed until it reopens.
            return closedStatus("This hospital is temporarily closed.");
        }
        const int manualMinutes = static_cast<int>((*secondsUntilManualClose + 59) / 60);  // round up
        minutesLeft = minutesLeft ? std::min(*minutesLeft, manualMinutes) : manualMinutes;
    }

    if (minutesLeft && *minutesLeft <= CLOSED_BEFORE_CLOSING_MINUTES) {
        HospitalStatus status = closedStatus(
            "This hospital is closing soon and is currently unavailable for new donor visits.");
        status.minutesUntilClose = *minutesLeft;
        return status;
    }
    if (minutesLeft && (manualClosing || *minutesLeft <= REGULAR_CLOSING_NOTICE_MINUTES)) {
        return {"closing_soon", "Closing soon",
                "This hospital will close in " + std::to_string(*minutesLeft) + " minutes.", *minutesLeft, true};
    }
    return {"open", "Open", "This hospital is open.", minutesLeft ? *minutesLeft : -1, true};
}

std::optional<int> parseClockTime(const std::string& text) {
    if (text.size() != 5 || text[2] != ':') {
        return std::nullopt;
    }
    for (int index : {0, 1, 3, 4}) {
        if (text[index] < '0' || text[index] > '9') {
            return std::nullopt;
        }
    }
    const int hour = (text[0] - '0') * 10 + (text[1] - '0');
    const int minute = (text[3] - '0') * 10 + (text[4] - '0');
    if (hour > 23 || minute > 59) {
        return std::nullopt;
    }
    return hour * 60 + minute;
}

std::string formatClockTime(int minuteOfDay) {
    const int minute = ((minuteOfDay % MINUTES_PER_DAY) + MINUTES_PER_DAY) % MINUTES_PER_DAY;
    const int hour = minute / 60;
    const int rest = minute % 60;
    return std::string{static_cast<char>('0' + hour / 10), static_cast<char>('0' + hour % 10), ':',
                       static_cast<char>('0' + rest / 10), static_cast<char>('0' + rest % 10)};
}

bool isValidClosingSoonMinutes(int minutes) {
    return minutes >= MIN_CLOSING_SOON_MINUTES && minutes <= MAX_CLOSING_SOON_MINUTES;
}

std::optional<HospitalState> parseHospitalState(const std::string& text) {
    if (text == "open") return HospitalState::Open;
    if (text == "closing_soon") return HospitalState::ClosingSoon;
    if (text == "temporarily_closed") return HospitalState::TemporarilyClosed;
    if (text == "permanently_closed") return HospitalState::PermanentlyClosed;
    return std::nullopt;
}

std::string hospitalStateName(HospitalState state) {
    switch (state) {
        case HospitalState::Open: return "open";
        case HospitalState::ClosingSoon: return "closing_soon";
        case HospitalState::TemporarilyClosed: return "temporarily_closed";
        case HospitalState::PermanentlyClosed: return "permanently_closed";
    }
    return "open";
}
