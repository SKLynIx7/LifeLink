#include "MatchRequest.h"
#include "BloodType.h"

namespace {
double calculatePriorityScore(const Donor& donor) {
    const double proximityScore = 1.0 -
        (donor.getDistanceKm() < NORMAL_SEARCH_RADIUS_KM
            ? donor.getDistanceKm() / NORMAL_SEARCH_RADIUS_KM
            : 1.0);
    const double reliabilityScore = donor.getReliabilityScore() / 5.0;

    return DISTANCE_WEIGHT * proximityScore + RELIABILITY_WEIGHT * reliabilityScore;
}
}

bool DonorPriority::operator()(const Donor& first, const Donor& second) const {
    const double firstScore = calculatePriorityScore(first);
    const double secondScore = calculatePriorityScore(second);

    if (firstScore != secondScore) {
        return firstScore < secondScore;
    }

    return first.getDistanceKm() > second.getDistanceKm();
}

MatchRequest::MatchRequest(int requestId, int hospitalId, const std::string& receiverName,
                           const std::string& requiredBloodType, int unitsRequired)
    : requestId(requestId), hospitalId(hospitalId), receiverName(receiverName),
      requiredBloodType(requiredBloodType), unitsRequired(unitsRequired), status("pending") {
}

bool MatchRequest::addCandidate(const Donor& donor) {
    if (!donor.isEligible() || !canDonateTo(donor.getBloodType(), requiredBloodType)) {
        return false;
    }

    rankedDonors.push(donor);
    status = "contacting_donors";
    return true;
}

std::optional<Donor> MatchRequest::getHighestRankedDonor() const {
    if (rankedDonors.empty() || status == "matched") {
        return std::nullopt;
    }

    return rankedDonors.top();
}

bool MatchRequest::recordResponse(int donorId, bool accepted) {
    if (rankedDonors.empty() || rankedDonors.top().getId() != donorId) {
        return false;
    }

    if (accepted) {
        matchedDonor = rankedDonors.top();
        status = "matched";
        return true;
    }

    rankedDonors.pop();
    status = rankedDonors.empty() ? "no_donors_found" : "contacting_donors";
    return true;
}

int MatchRequest::getRequestId() const {
    return requestId;
}

int MatchRequest::getHospitalId() const {
    return hospitalId;
}

const std::string& MatchRequest::getReceiverName() const {
    return receiverName;
}

const std::string& MatchRequest::getRequiredBloodType() const {
    return requiredBloodType;
}

int MatchRequest::getUnitsRequired() const {
    return unitsRequired;
}

const std::string& MatchRequest::getStatus() const {
    return status;
}

std::size_t MatchRequest::getEligibleCandidateCount() const {
    return rankedDonors.size();
}

std::optional<Donor> MatchRequest::getMatchedDonor() const {
    return matchedDonor;
}
