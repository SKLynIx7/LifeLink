#ifndef MATCH_REQUEST_H
#define MATCH_REQUEST_H

#include "Donor.h"

#include <cstddef>
#include <deque>
#include <optional>
#include <queue>
#include <string>

constexpr double NORMAL_SEARCH_RADIUS_KM = 20.0;
constexpr double DISTANCE_WEIGHT = 0.65;
constexpr double RELIABILITY_WEIGHT = 0.35;
// Donors rated below this are still listed, but flagged as low reliability.
// New donors start at 5.0, which also counts as one rating in their average.
constexpr double LOW_RELIABILITY_THRESHOLD = 3.0;
// The top donors in the ranking are marked "recommended" for the receiver.
constexpr std::size_t RECOMMENDED_DONOR_COUNT = 3;

// priorityScore = 0.65 x proximity + 0.35 x (rating / 5)^2, in the range 0..1.
double calculatePriorityScore(const Donor& donor);
bool isLowReliability(const Donor& donor);

// Comparator for std::priority_queue. The queue keeps the donor with the
// HIGHEST priority score on top, so it is a max-heap on score (equivalently a
// min-heap on "cost" = 1 - score). Ties go to the nearer donor.
struct DonorPriority {
    bool operator()(const Donor& first, const Donor& second) const;
};

// Ranks the compatible, eligible donors for one blood request.
// The receiver chooses whom to invite from this ranking.
class MatchRequest {
public:
    MatchRequest(int requestId, int hospitalId, const std::string& requiredBloodType,
                 double searchRadiusKm = NORMAL_SEARCH_RADIUS_KM);

    // Adds the donor only if eligible, blood-compatible and inside the search radius.
    bool addCandidate(const Donor& donor);

    std::optional<Donor> getHighestRankedDonor() const;
    // Removes the current best donor so the next one rises to the top.
    std::optional<Donor> popHighestRankedDonor();
    // Donors in ranking order (best first), without changing the heap.
    // limit = 0 returns every candidate.
    std::deque<Donor> getRankedDonors(std::size_t limit = 0) const;

    int getRequestId() const;
    int getHospitalId() const;
    const std::string& getRequiredBloodType() const;
    double getSearchRadiusKm() const;
    std::size_t getEligibleCandidateCount() const;

private:
    int requestId;
    int hospitalId;
    std::string requiredBloodType;
    double searchRadiusKm;
    std::priority_queue<Donor, std::deque<Donor>, DonorPriority> rankedDonors;
};

#endif
