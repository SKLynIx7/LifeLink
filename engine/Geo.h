#ifndef GEO_H
#define GEO_H

#include <algorithm>
#include <cmath>

// Great-circle distance between two latitude/longitude points (haversine formula).
// Unlike a flat "degrees x 111 km" estimate, this accounts for longitude lines
// getting closer together away from the equator.
inline double haversineKm(double latitude1, double longitude1, double latitude2, double longitude2) {
    constexpr double earthRadiusKm = 6371.0;
    constexpr double degreesToRadians = 3.14159265358979323846 / 180.0;

    const double latitudeDelta = (latitude2 - latitude1) * degreesToRadians;
    const double longitudeDelta = (longitude2 - longitude1) * degreesToRadians;
    const double a = std::sin(latitudeDelta / 2.0) * std::sin(latitudeDelta / 2.0) +
                     std::cos(latitude1 * degreesToRadians) * std::cos(latitude2 * degreesToRadians) *
                     std::sin(longitudeDelta / 2.0) * std::sin(longitudeDelta / 2.0);

    return 2.0 * earthRadiusKm * std::asin(std::min(1.0, std::sqrt(a)));
}

#endif
