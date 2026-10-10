#ifndef MATCH_REQUEST_H
#define MATCH_REQUEST_H

#include "Donor.h"

#include <cstddef>
#include <deque>
#include <optional>
#include <queue>
#include <string>

constexpr double NORMAL_SEARCH_RADIUS_KM = 20.0;
constexpr double DISTANCE_WEIGHT = 0.60;
constexpr double RELIABILITY_WEIGHT = 0.40;
constexpr double NEARBY_DISTANCE_THRESHOLD_KM = 3.0;
constexpr double LOW_RELIABILITY_THRESHOLD = 3.0;

struct DonorPriority {
    bool operator()(const Donor& first, const Donor& second) const;
};

class MatchRequest {
public:
    MatchRequest(int requestId, int hospitalId, const std::string& receiverName,
                 const std::string& requiredBloodType, int unitsRequired);

    bool addCandidate(const Donor& donor);
    std::optional<Donor> getHighestRankedDonor() const;
    bool recordResponse(int donorId, bool accepted);

    int getRequestId() const;
    int getHospitalId() const;
    const std::string& getReceiverName() const;
    const std::string& getRequiredBloodType() const;
    int getUnitsRequired() const;
    const std::string& getStatus() const;
    std::size_t getEligibleCandidateCount() const;
    std::optional<Donor> getMatchedDonor() const;

private:
    int requestId;
    int hospitalId;
    std::string receiverName;
    std::string requiredBloodType;
    int unitsRequired;
    std::string status;
    std::priority_queue<Donor, std::deque<Donor>, DonorPriority> rankedDonors;
    std::optional<Donor> matchedDonor;
};

#endif
