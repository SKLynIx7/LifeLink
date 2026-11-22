// Unit tests for hospital availability: regular hours, working days, overnight shifts,
// the hospital's own "closing soon" / "closed" settings, and the 30-minute rule.

#include "TestFramework.h"

#include "../engine/HospitalStatus.h"

namespace {
constexpr int MON = 0, TUE = 1, SAT = 5, SUN = 6;

OpeningHours hours(const char* open, const char* close, std::array<bool, 7> days = {{true, true, true, true, true, true, true}}) {
    return OpeningHours{*parseClockTime(open), *parseClockTime(close), days};
}

LocalTime at(int weekday, const char* time) {
    return LocalTime{weekday, *parseClockTime(time)};
}

const OpeningHours ALL_DAY = hours("00:00", "00:00");
const OpeningHours NINE_TO_EIGHT = hours("09:00", "20:00");

HospitalStatus openAt(const OpeningHours& h, LocalTime now) {
    return evaluateHospitalStatus(HospitalState::Open, std::nullopt, h, now);
}

HospitalStatus closingInSeconds(long long seconds, const OpeningHours& h = ALL_DAY, LocalTime now = at(MON, "12:00")) {
    return evaluateHospitalStatus(HospitalState::ClosingSoon, seconds, h, now);
}
}  // namespace

// ---------------- Time parsing ----------------

TEST(clock_times_parse_and_format) {
    CHECK(parseClockTime("00:00") == 0);
    CHECK(parseClockTime("09:30") == 570);
    CHECK(parseClockTime("23:59") == 1439);
    CHECK(!parseClockTime("24:00"));
    CHECK(!parseClockTime("12:60"));
    CHECK(!parseClockTime("9:30"));
    CHECK(!parseClockTime("09-30"));
    CHECK(!parseClockTime("ab:cd"));
    CHECK(formatClockTime(570) == "09:30");
    CHECK(formatClockTime(0) == "00:00");
}

TEST(closing_soon_minutes_must_be_1_to_720) {
    CHECK(!isValidClosingSoonMinutes(0));
    CHECK(isValidClosingSoonMinutes(1));
    CHECK(isValidClosingSoonMinutes(45));
    CHECK(isValidClosingSoonMinutes(720));
    CHECK(!isValidClosingSoonMinutes(721));
    CHECK(!isValidClosingSoonMinutes(-5));
}

TEST(hospital_state_names_round_trip) {
    for (HospitalState state : {HospitalState::Open, HospitalState::ClosingSoon,
                                HospitalState::TemporarilyClosed, HospitalState::PermanentlyClosed}) {
        CHECK(parseHospitalState(hospitalStateName(state)) == state);
    }
    CHECK(!parseHospitalState("closed"));
    CHECK(!parseHospitalState("OPEN"));
}

// ---------------- The hospital's own setting ----------------

TEST(open_all_day_is_open) {
    const HospitalStatus status = openAt(ALL_DAY, at(MON, "03:00"));
    CHECK(status.code == "open" && status.badge == "Open");
    CHECK(status.acceptsNewDonations);
    CHECK(status.minutesUntilClose == -1);
}

TEST(temporarily_closed_is_closed) {
    const HospitalStatus status = evaluateHospitalStatus(HospitalState::TemporarilyClosed, std::nullopt, ALL_DAY, at(MON, "12:00"));
    CHECK(status.code == "closed" && status.badge == "Closed");
    CHECK(status.message == "This hospital is temporarily closed.");
    CHECK(!status.acceptsNewDonations);
}

TEST(permanently_closed_has_its_own_badge) {
    const HospitalStatus status = evaluateHospitalStatus(HospitalState::PermanentlyClosed, std::nullopt, ALL_DAY, at(MON, "12:00"));
    CHECK(status.code == "permanently_closed" && status.badge == "Permanently Closed");
    CHECK(!status.acceptsNewDonations);
}

// ---------------- The 30-minute rule ----------------

TEST(closing_in_more_than_30_minutes_is_closing_soon) {
    const HospitalStatus status = closingInSeconds(45 * 60);
    CHECK(status.code == "closing_soon" && status.badge == "Closing soon");
    CHECK(status.message == "This hospital will close in 45 minutes.");
    CHECK(status.minutesUntilClose == 45);
    CHECK(status.acceptsNewDonations);
}

TEST(closing_in_31_minutes_is_still_closing_soon) {
    CHECK(closingInSeconds(31 * 60).code == "closing_soon");
    CHECK(closingInSeconds(30 * 60 + 1).code == "closing_soon");  // 30 min 1 s rounds up to 31
}

TEST(closing_in_exactly_30_minutes_is_closed) {
    const HospitalStatus status = closingInSeconds(30 * 60);
    CHECK(status.code == "closed" && status.badge == "Closed");
    CHECK(status.message == "This hospital is closing soon and is currently unavailable for new donor visits.");
    CHECK(status.minutesUntilClose == 30);
    CHECK(!status.acceptsNewDonations);
}

TEST(closing_within_30_minutes_is_closed_never_closing_soon) {
    for (long long minutes : {29LL, 15LL, 1LL}) {
        const HospitalStatus status = closingInSeconds(minutes * 60);
        CHECK(status.code == "closed");
        CHECK(status.badge != "Closing soon");
        CHECK(!status.acceptsNewDonations);
    }
}

TEST(closing_time_already_passed_is_temporarily_closed) {
    CHECK(closingInSeconds(0).message == "This hospital is temporarily closed.");
    CHECK(closingInSeconds(-600).code == "closed");
    CHECK(evaluateHospitalStatus(HospitalState::ClosingSoon, std::nullopt, ALL_DAY, at(MON, "12:00")).code == "closed");
}

TEST(closing_soon_up_to_12_hours_ahead_shows_the_countdown) {
    const HospitalStatus status = closingInSeconds(720LL * 60);
    CHECK(status.code == "closing_soon");
    CHECK(status.minutesUntilClose == 720);
}

// ---------------- Regular hours ----------------

TEST(inside_regular_hours_is_open) {
    const HospitalStatus status = openAt(NINE_TO_EIGHT, at(TUE, "12:00"));
    CHECK(status.code == "open");
    CHECK(status.minutesUntilClose == 480);
}

TEST(before_opening_time_is_closed_with_opening_time) {
    const HospitalStatus status = openAt(NINE_TO_EIGHT, at(TUE, "07:15"));
    CHECK(status.code == "closed" && !status.acceptsNewDonations);
    CHECK(status.message == "This hospital is closed now. It opens today at 09:00.");
}

TEST(after_closing_time_opens_tomorrow) {
    const HospitalStatus status = openAt(NINE_TO_EIGHT, at(TUE, "21:00"));
    CHECK(status.code == "closed");
    CHECK(status.message == "This hospital is closed now. It opens tomorrow at 09:00.");
}

TEST(non_working_day_names_the_next_working_day) {
    // Monday to Friday only: on Saturday it next opens on Monday.
    const OpeningHours weekdays = hours("09:00", "17:00", {{true, true, true, true, true, false, false}});
    const HospitalStatus status = openAt(weekdays, at(SAT, "11:00"));
    CHECK(status.code == "closed");
    CHECK(status.message == "This hospital is closed now. It opens on Monday at 09:00.");
}

TEST(last_hour_of_regular_hours_is_closing_soon) {
    const HospitalStatus status = openAt(NINE_TO_EIGHT, at(TUE, "19:00"));  // closes 20:00
    CHECK(status.code == "closing_soon");
    CHECK(status.minutesUntilClose == 60);
    CHECK(openAt(NINE_TO_EIGHT, at(TUE, "18:59")).code == "open");  // 61 minutes left
}

TEST(last_30_minutes_of_regular_hours_is_closed) {
    const HospitalStatus status = openAt(NINE_TO_EIGHT, at(TUE, "19:30"));
    CHECK(status.code == "closed");
    CHECK(status.message == "This hospital is closing soon and is currently unavailable for new donor visits.");
    CHECK(openAt(NINE_TO_EIGHT, at(TUE, "19:29")).code == "closing_soon");  // 31 minutes left
}

TEST(overnight_shift_is_open_after_midnight) {
    const OpeningHours night = hours("20:00", "08:00");
    CHECK(openAt(night, at(TUE, "23:00")).code == "open");
    const HospitalStatus early = openAt(night, at(TUE, "02:00"));  // Monday's shift, until 08:00
    CHECK(early.code == "open");
    CHECK(early.minutesUntilClose == 360);
    CHECK(openAt(night, at(TUE, "12:00")).code == "closed");
}

TEST(overnight_shift_across_sunday_midnight) {
    // Works Sunday night only: Monday 02:00 is still inside Sunday's shift.
    const OpeningHours sundayNight = hours("22:00", "06:00", {{false, false, false, false, false, false, true}});
    CHECK(openAt(sundayNight, at(MON, "02:00")).code == "open");
    CHECK(openAt(sundayNight, at(MON, "07:00")).code == "closed");
    CHECK(openAt(sundayNight, at(SUN, "23:00")).code == "open");
}

TEST(all_day_hospital_closes_at_midnight_before_a_day_off) {
    // Open 24 hours Monday to Saturday: Saturday 23:15 is 45 minutes before closing.
    const OpeningHours monToSat = hours("00:00", "00:00", {{true, true, true, true, true, true, false}});
    const HospitalStatus status = openAt(monToSat, at(SAT, "23:15"));
    CHECK(status.code == "closing_soon");
    CHECK(status.minutesUntilClose == 45);
    CHECK(openAt(monToSat, at(SUN, "10:00")).code == "closed");
    CHECK(openAt(monToSat, at(TUE, "23:50")).code == "open");  // the next day is a working day
}

TEST(earlier_of_manual_and_regular_closing_wins) {
    // Set to close in 3 hours, but regular hours end in 50 minutes.
    const HospitalStatus regularFirst = evaluateHospitalStatus(HospitalState::ClosingSoon, 180LL * 60, NINE_TO_EIGHT, at(TUE, "19:10"));
    CHECK(regularFirst.code == "closing_soon" && regularFirst.minutesUntilClose == 50);
    // Set to close in 20 minutes during the day: closed now.
    const HospitalStatus manualFirst = evaluateHospitalStatus(HospitalState::ClosingSoon, 20LL * 60, NINE_TO_EIGHT, at(TUE, "12:00"));
    CHECK(manualFirst.code == "closed" && manualFirst.minutesUntilClose == 20);
}

TEST(closing_soon_outside_regular_hours_is_closed) {
    const HospitalStatus status = evaluateHospitalStatus(HospitalState::ClosingSoon, 90LL * 60, NINE_TO_EIGHT, at(TUE, "22:00"));
    CHECK(status.code == "closed");
    CHECK(status.message == "This hospital is closed now. It opens tomorrow at 09:00.");
}

TEST(no_working_days_is_always_closed) {
    const OpeningHours none = hours("09:00", "17:00", {{false, false, false, false, false, false, false}});
    const HospitalStatus status = openAt(none, at(MON, "10:00"));
    CHECK(status.code == "closed");
    CHECK(status.message == "This hospital is closed now.");
}
