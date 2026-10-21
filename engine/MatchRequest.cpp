#include "MatchRequest.h"
#include "BloodType.h"

double calculatePriorityScore(const Donor& donor) {
    const double proximityScore = 1.0 -
        (donor.getDistanceKm() < NORMAL_SEARCH_RADIUS_KM
            ? donor.getDistanceKm() / NORMAL_SEARCH_RADIUS_KM
            : 1.0);
    const double normalizedReliability = donor.getReliabilityScore() / 5.0;
    const double reliabilityScore = normalizedReliability * normalizedReliability;

    return DISTANCE_WEIGHT * proximityScore + RELIABILITY_WEIGHT * reliabilityScore;
}

bool isLowReliability(const Donor& donor) {
    return donor.getReliabilityScore() < LOW_RELIABILITY_THRESHOLD;
}

bool DonorPriority::operator()(const Donor& first, const Donor& second) const {
    const double firstScore = calculatePriorityScore(first);
    const double secondScore = calculatePriorityScore(second);

    if (firstScore != secondScore) {
        return firstScore < secondScore;
    }

    return first.getDistanceKm() > second.getDistanceKm();
}

MatchRequest::MatchRequest(int requestId, int hospitalId, const std::string& requiredBloodType,
                           double searchRadiusKm)
    : requestId(requestId), hospitalId(hospitalId), requiredBloodType(requiredBloodType),
      searchRadiusKm(searchRadiusKm) {
}

bool MatchRequest::addCandidate(const Donor& donor) {
    if (!donor.isEligible() || !canDonateTo(donor.getBloodType(), requiredBloodType) ||
        donor.getDistanceKm() > searchRadiusKm) {
        return false;
    }

    rankedDonors.push(donor);
    return true;
}

std::optional<Donor> MatchRequest::getHighestRankedDonor() const {
    if (rankedDonors.empty()) {
        return std::nullopt;
    }
    return rankedDonors.top();
}

std::optional<Donor> MatchRequest::popHighestRankedDonor() {
    if (rankedDonors.empty()) {
        return std::nullopt;
    }
    Donor best = rankedDonors.top();
    rankedDonors.pop();
    return best;
}

std::deque<Donor> MatchRequest::getRankedDonors(std::size_t limit) const {
    // Pop from a copy of the heap: each pop gives the next best donor,
    // so the result is in ranking order and the original heap is unchanged.
    auto heap = rankedDonors;
    std::deque<Donor> ordered;
    while (!heap.empty() && (limit == 0 || ordered.size() < limit)) {
        ordered.push_back(heap.top());
        heap.pop();
    }
    return ordered;
}

int MatchRequest::getRequestId() const {
    return requestId;
}

int MatchRequest::getHospitalId() const {
    return hospitalId;
}

const std::string& MatchRequest::getRequiredBloodType() const {
    return requiredBloodType;
}

double MatchRequest::getSearchRadiusKm() const {
    return searchRadiusKm;
}

std::size_t MatchRequest::getEligibleCandidateCount() const {
    return rankedDonors.size();
}
