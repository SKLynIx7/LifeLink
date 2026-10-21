#ifndef BLOOD_TYPE_H
#define BLOOD_TYPE_H

#include <array>
#include <cstddef>
#include <string>
#include <unordered_map>

struct CompatibleDonorTypes {
    std::array<std::string, 8> bloodTypes;
    std::size_t count;
};

// Key: receiver blood type. Value: donor blood types that can donate to it.
inline const std::unordered_map<std::string, CompatibleDonorTypes>& getCompatibilityChart() {
    static const std::unordered_map<std::string, CompatibleDonorTypes> chart = {
        {"O-",  {{{"O-", "", "", "", "", "", "", ""}}, 1}},
        {"O+",  {{{"O+", "O-", "", "", "", "", "", ""}}, 2}},
        {"A-",  {{{"A-", "O-", "", "", "", "", "", ""}}, 2}},
        {"A+",  {{{"A+", "A-", "O+", "O-", "", "", "", ""}}, 4}},
        {"B-",  {{{"B-", "O-", "", "", "", "", "", ""}}, 2}},
        {"B+",  {{{"B+", "B-", "O+", "O-", "", "", "", ""}}, 4}},
        {"AB-", {{{"AB-", "A-", "B-", "O-", "", "", "", ""}}, 4}},
        // AB+ is the universal recipient: every blood group can donate to it.
        {"AB+", {{{"AB+", "AB-", "A+", "A-", "B+", "B-", "O+", "O-"}}, 8}}
    };

    return chart;
}

inline bool isValidBloodType(const std::string& bloodType) {
    return getCompatibilityChart().find(bloodType) != getCompatibilityChart().end();
}

inline bool canDonateTo(const std::string& donorBloodType, const std::string& receiverBloodType) {
    const auto receiver = getCompatibilityChart().find(receiverBloodType);

    if (receiver == getCompatibilityChart().end()) {
        return false;
    }

    for (std::size_t index = 0; index < receiver->second.count; ++index) {
        if (receiver->second.bloodTypes[index] == donorBloodType) {
            return true;
        }
    }

    return false;
}

#endif
