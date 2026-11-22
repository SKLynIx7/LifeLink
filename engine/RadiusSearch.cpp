#include "RadiusSearch.h"

#include <cstdlib>

bool isAllowedSearchRadius(double radiusKm) {
    for (double step : SEARCH_RADIUS_STEPS_KM) {
        if (radiusKm == step) {
            return true;
        }
    }
    return false;
}

std::optional<RadiusChoice> parseRadiusChoice(const std::string& value) {
    if (value.empty() || value == "auto") {
        return RadiusChoice{true, 0.0};
    }

    // Whole numbers only: "10" is accepted, "10.5", "10km" and "-5" are not.
    char* end = nullptr;
    const long number = std::strtol(value.c_str(), &end, 10);
    if (end == value.c_str() || *end != '\0') {
        return std::nullopt;
    }
    const double radiusKm = static_cast<double>(number);
    if (!isAllowedSearchRadius(radiusKm)) {
        return std::nullopt;
    }
    return RadiusChoice{false, radiusKm};
}

RadiusSearchResult searchFixedRadius(int requestId, int hospitalId, const std::string& bloodType,
                                     double radiusKm, const std::vector<Donor>& donorPool) {
    RadiusSearchResult result{MatchRequest(requestId, hospitalId, bloodType, radiusKm), {}, false};
    for (const Donor& donor : donorPool) {
        result.match.addCandidate(donor);  // filters eligibility, blood group and radius
    }
    result.radiiTried.push_back(result.match.getSearchRadiusKm());
    return result;
}

RadiusSearchResult searchWithAutoExpand(int requestId, int hospitalId, const std::string& bloodType,
                                        const std::vector<Donor>& donorPool) {
    std::vector<double> tried;
    for (double radiusKm : SEARCH_RADIUS_STEPS_KM) {
        RadiusSearchResult step = searchFixedRadius(requestId, hospitalId, bloodType, radiusKm, donorPool);
        tried.push_back(radiusKm);
        const bool lastStep = radiusKm == SEARCH_RADIUS_STEPS_KM.back();
        if (step.match.getEligibleCandidateCount() > 0 || lastStep) {
            step.radiiTried = tried;
            step.autoExpanded = true;
            return step;
        }
    }
    // Not reached: the loop always returns at the last step.
    return searchFixedRadius(requestId, hospitalId, bloodType, MAX_SEARCH_RADIUS_KM, donorPool);
}

RadiusSearchResult runRadiusSearch(int requestId, int hospitalId, const std::string& bloodType,
                                   const RadiusChoice& choice, const std::vector<Donor>& donorPool) {
    return choice.autoExpand
        ? searchWithAutoExpand(requestId, hospitalId, bloodType, donorPool)
        : searchFixedRadius(requestId, hospitalId, bloodType, choice.radiusKm, donorPool);
}
