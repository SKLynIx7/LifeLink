#ifndef RADIUS_SEARCH_H
#define RADIUS_SEARCH_H

#include "MatchRequest.h"

#include <array>
#include <optional>
#include <string>
#include <vector>

// The receiver can search 5, 10, 15, 20 or 25 km around the hospital, or let
// LifeLink widen the search step by step ("auto-expand"). 25 km is the limit.
constexpr std::array<double, 5> SEARCH_RADIUS_STEPS_KM = {5.0, 10.0, 15.0, 20.0, 25.0};

// True only for 5, 10, 15, 20 and 25. Anything else (including > 25) is rejected.
bool isAllowedSearchRadius(double radiusKm);

// Reads the receiver's choice: "auto" (or nothing) means auto-expand,
// "5", "10", "15", "20" or "25" means that fixed radius. Anything else returns nullopt.
struct RadiusChoice {
    bool autoExpand;
    double radiusKm;  // used only when autoExpand is false
};
std::optional<RadiusChoice> parseRadiusChoice(const std::string& value);

struct RadiusSearchResult {
    MatchRequest match;              // ranked donors at the final radius
    std::vector<double> radiiTried;  // e.g. {5, 10, 15} when 15 km was the first with a donor
    bool autoExpanded;
};

// One fixed radius: donors from the pool within that radius, ranked.
RadiusSearchResult searchFixedRadius(int requestId, int hospitalId, const std::string& bloodType,
                                     double radiusKm, const std::vector<Donor>& donorPool);

// Tries 5, 10, 15, 20 and 25 km in turn and stops at the first radius that has
// at least one eligible, compatible donor. Never goes beyond 25 km.
RadiusSearchResult searchWithAutoExpand(int requestId, int hospitalId, const std::string& bloodType,
                                        const std::vector<Donor>& donorPool);

// Runs whichever of the two the choice asks for.
RadiusSearchResult runRadiusSearch(int requestId, int hospitalId, const std::string& bloodType,
                                   const RadiusChoice& choice, const std::vector<Donor>& donorPool);

#endif
