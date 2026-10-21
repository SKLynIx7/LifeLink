#include "Donor.h"

Donor::Donor(int id, const std::string& name, const std::string& phone,
             const std::string& bloodType, double distanceKm,
             int daysSinceLastDonation, double reliabilityScore, bool available)
    : id(id), name(name), phone(phone), bloodType(bloodType),
      distanceKm(distanceKm), daysSinceLastDonation(daysSinceLastDonation),
      reliabilityScore(reliabilityScore), available(available) {
}

int Donor::getId() const {
    return id;
}

const std::string& Donor::getName() const {
    return name;
}

const std::string& Donor::getPhone() const {
    return phone;
}

const std::string& Donor::getBloodType() const {
    return bloodType;
}

double Donor::getDistanceKm() const {
    return distanceKm;
}

int Donor::getDaysSinceLastDonation() const {
    return daysSinceLastDonation;
}

double Donor::getReliabilityScore() const {
    return reliabilityScore;
}

bool Donor::isAvailable() const {
    return available;
}

bool Donor::isEligible() const {
    return available && daysSinceLastDonation >= MIN_DAYS_BETWEEN_DONATIONS;
}

void Donor::setAvailable(bool value) {
    available = value;
}

void Donor::setDistanceKm(double value) {
    distanceKm = value;
}

void Donor::setDaysSinceLastDonation(int days) {
    daysSinceLastDonation = days;
}

void Donor::setReliabilityScore(double score) {
    reliabilityScore = score;
}
