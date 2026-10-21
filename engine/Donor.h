#ifndef DONOR_H
#define DONOR_H

#include <string>

// Minimum gap between two whole-blood donations.
constexpr int MIN_DAYS_BETWEEN_DONATIONS = 90;

class Donor {
public:
    Donor(int id, const std::string& name, const std::string& phone,
          const std::string& bloodType, double distanceKm,
          int daysSinceLastDonation, double reliabilityScore,
          bool available = true);

    int getId() const;
    const std::string& getName() const;
    const std::string& getPhone() const;
    const std::string& getBloodType() const;
    double getDistanceKm() const;
    int getDaysSinceLastDonation() const;
    double getReliabilityScore() const;
    bool isAvailable() const;
    bool isEligible() const;

    void setAvailable(bool available);
    void setDistanceKm(double distanceKm);
    void setDaysSinceLastDonation(int days);
    void setReliabilityScore(double score);

private:
    int id;
    std::string name;
    std::string phone;
    std::string bloodType;
    double distanceKm;
    int daysSinceLastDonation;
    double reliabilityScore;
    bool available;
};

#endif
