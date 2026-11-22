// Unit tests for the search radius: allowed values, auto-expand 5 -> 25 km,
// fixed radius, and the 25 km limit.

#include "TestFramework.h"

#include "../engine/Geo.h"
#include "../engine/MatchRequest.h"
#include "../engine/RadiusSearch.h"

#include <vector>

namespace {
// An eligible A+ donor (available, last donated 200 days ago, rating 4.5) at a given distance.
Donor donorAt(int id, double distanceKm) {
    return Donor(id, "Donor " + std::to_string(id), "9876543210", "A+", distanceKm, 200, 4.5);
}

RadiusSearchResult autoSearch(const std::vector<Donor>& pool) {
    return searchWithAutoExpand(1, 1, "A+", pool);
}

bool everyDonorWithin(const RadiusSearchResult& result) {
    for (const Donor& donor : result.match.getRankedDonors()) {
        if (donor.getDistanceKm() > result.match.getSearchRadiusKm()) {
            return false;
        }
    }
    return true;
}
}  // namespace

// ---------------- Allowed values ----------------

TEST(only_5_10_15_20_25_are_allowed) {
    CHECK(isAllowedSearchRadius(5));
    CHECK(isAllowedSearchRadius(10));
    CHECK(isAllowedSearchRadius(15));
    CHECK(isAllowedSearchRadius(20));
    CHECK(isAllowedSearchRadius(25));
    CHECK(!isAllowedSearchRadius(0));
    CHECK(!isAllowedSearchRadius(7));
    CHECK(!isAllowedSearchRadius(25.5));
    CHECK(!isAllowedSearchRadius(30));
    CHECK(!isAllowedSearchRadius(-5));
}

TEST(parse_auto_and_fixed_choices) {
    const auto empty = parseRadiusChoice("");
    CHECK(empty.has_value() && empty->autoExpand);

    const auto automatic = parseRadiusChoice("auto");
    CHECK(automatic.has_value() && automatic->autoExpand);

    const auto ten = parseRadiusChoice("10");
    CHECK(ten.has_value() && !ten->autoExpand && ten->radiusKm == 10.0);
}

TEST(parse_rejects_invalid_and_too_large_values) {
    CHECK(!parseRadiusChoice("30").has_value());     // above the 25 km limit
    CHECK(!parseRadiusChoice("100").has_value());
    CHECK(!parseRadiusChoice("7").has_value());      // not one of the steps
    CHECK(!parseRadiusChoice("-5").has_value());
    CHECK(!parseRadiusChoice("10.5").has_value());
    CHECK(!parseRadiusChoice("10km").has_value());
    CHECK(!parseRadiusChoice("abc").has_value());
    CHECK(!parseRadiusChoice("AUTO").has_value());
}

// ---------------- Auto-expand ----------------

TEST(auto_expand_stops_at_5_km_when_a_donor_is_close) {
    const RadiusSearchResult result = autoSearch({donorAt(1, 3.0)});
    CHECK(result.autoExpanded);
    CHECK(result.match.getSearchRadiusKm() == 5.0);
    CHECK(result.radiiTried == std::vector<double>({5.0}));
    CHECK(result.match.getEligibleCandidateCount() == 1);
}

TEST(auto_expand_widens_5_10_15_until_a_donor_is_found) {
    const RadiusSearchResult result = autoSearch({donorAt(1, 12.0)});
    CHECK(result.match.getSearchRadiusKm() == 15.0);
    CHECK(result.radiiTried == std::vector<double>({5.0, 10.0, 15.0}));
    CHECK(result.match.getEligibleCandidateCount() == 1);
}

TEST(auto_expand_reaches_25_km_for_a_far_donor) {
    const RadiusSearchResult result = autoSearch({donorAt(1, 24.0)});
    CHECK(result.match.getSearchRadiusKm() == 25.0);
    CHECK(result.radiiTried == std::vector<double>({5.0, 10.0, 15.0, 20.0, 25.0}));
    CHECK(result.match.getEligibleCandidateCount() == 1);
}

TEST(auto_expand_never_goes_beyond_25_km) {
    // The only donor is 30 km away: the search ends at 25 km with no result.
    const RadiusSearchResult result = autoSearch({donorAt(1, 30.0)});
    CHECK(result.match.getSearchRadiusKm() == 25.0);
    CHECK(result.radiiTried.size() == 5);
    CHECK(result.radiiTried.back() == 25.0);
    CHECK(result.match.getEligibleCandidateCount() == 0);
}

TEST(auto_expand_with_no_donors_ends_empty_at_25_km) {
    const RadiusSearchResult result = autoSearch({});
    CHECK(result.match.getSearchRadiusKm() == 25.0);
    CHECK(result.radiiTried.size() == 5);
    CHECK(result.match.getEligibleCandidateCount() == 0);
}

TEST(auto_expand_stops_at_the_first_radius_and_excludes_farther_donors) {
    // Donors at 8 km and 18 km: the search stops at 10 km and shows only the 8 km donor.
    const RadiusSearchResult result = autoSearch({donorAt(1, 18.0), donorAt(2, 8.0)});
    CHECK(result.match.getSearchRadiusKm() == 10.0);
    CHECK(result.match.getEligibleCandidateCount() == 1);
    CHECK(result.match.getHighestRankedDonor()->getId() == 2);
    CHECK(everyDonorWithin(result));
}

TEST(auto_expand_ignores_ineligible_donors_nearby) {
    // Close donors who cannot give (wrong group, donated recently, unavailable)
    // do not stop the search; it keeps widening to the eligible donor at 14 km.
    const std::vector<Donor> pool = {
        Donor(1, "Incompatible", "9876543210", "B+", 2.0, 200, 5.0),
        Donor(2, "Donated recently", "9876543211", "A+", 3.0, 30, 5.0),
        Donor(3, "Unavailable", "9876543212", "A+", 4.0, 200, 5.0, false),
        donorAt(4, 14.0),
    };
    const RadiusSearchResult result = autoSearch(pool);
    CHECK(result.match.getSearchRadiusKm() == 15.0);
    CHECK(result.match.getEligibleCandidateCount() == 1);
    CHECK(result.match.getHighestRankedDonor()->getId() == 4);
}

TEST(a_donor_exactly_on_the_boundary_is_included) {
    const RadiusSearchResult result = autoSearch({donorAt(1, 5.0)});
    CHECK(result.match.getSearchRadiusKm() == 5.0);
    CHECK(result.match.getEligibleCandidateCount() == 1);
}

// ---------------- Fixed radius ----------------

TEST(fixed_radius_returns_only_donors_inside_it) {
    const RadiusSearchResult result =
        searchFixedRadius(1, 1, "A+", 10.0, {donorAt(1, 4.0), donorAt(2, 9.0), donorAt(3, 11.0)});
    CHECK(!result.autoExpanded);
    CHECK(result.match.getSearchRadiusKm() == 10.0);
    CHECK(result.radiiTried == std::vector<double>({10.0}));
    CHECK(result.match.getEligibleCandidateCount() == 2);
    CHECK(everyDonorWithin(result));
}

TEST(fixed_radius_does_not_expand_when_empty) {
    const RadiusSearchResult result = searchFixedRadius(1, 1, "A+", 5.0, {donorAt(1, 12.0)});
    CHECK(result.match.getSearchRadiusKm() == 5.0);
    CHECK(result.match.getEligibleCandidateCount() == 0);
}

TEST(run_radius_search_follows_the_choice) {
    const std::vector<Donor> pool = {donorAt(1, 12.0)};
    const RadiusSearchResult automatic = runRadiusSearch(1, 1, "A+", RadiusChoice{true, 0.0}, pool);
    const RadiusSearchResult fixed = runRadiusSearch(1, 1, "A+", RadiusChoice{false, 20.0}, pool);
    CHECK(automatic.autoExpanded && automatic.match.getSearchRadiusKm() == 15.0);
    CHECK(!fixed.autoExpanded && fixed.match.getSearchRadiusKm() == 20.0);
}

// ---------------- The 25 km limit in the engine ----------------

TEST(match_request_clamps_a_radius_above_25_km) {
    MatchRequest request(1, 1, "A+", 40.0);
    CHECK(request.getSearchRadiusKm() == 25.0);
    CHECK(!request.addCandidate(donorAt(1, 30.0)));
    CHECK(request.addCandidate(donorAt(2, 24.0)));
}

TEST(priority_score_uses_the_25_km_scale) {
    // proximity = 1 - distance / 25; rating 5 gives the full reliability part.
    CHECK_NEAR(calculatePriorityScore(Donor(1, "A", "9876543210", "A+", 0.0, 200, 5.0)), 1.0);
    CHECK_NEAR(calculatePriorityScore(Donor(2, "B", "9876543210", "A+", 12.5, 200, 5.0)), 0.65 * 0.5 + 0.35);
    CHECK_NEAR(calculatePriorityScore(Donor(3, "C", "9876543210", "A+", 25.0, 200, 5.0)), 0.35);
}

// ---------------- Bounding box used by the database query ----------------

TEST(bounding_box_contains_the_whole_25_km_circle) {
    const double latitude = 30.3165;   // Dehradun
    const double longitude = 78.0322;
    const BoundingBox box = boundingBoxAround(latitude, longitude, 25.0);

    // The box edges are at least 25 km away, so no donor within 25 km is cut off.
    CHECK(haversineKm(latitude, longitude, box.maxLatitude, longitude) >= 25.0);
    CHECK(haversineKm(latitude, longitude, box.minLatitude, longitude) >= 25.0);
    CHECK(haversineKm(latitude, longitude, latitude, box.maxLongitude) >= 25.0);
    CHECK(haversineKm(latitude, longitude, latitude, box.minLongitude) >= 25.0);
    // And it is not much larger than needed.
    CHECK(haversineKm(latitude, longitude, box.maxLatitude, longitude) <= 25.5);
}
