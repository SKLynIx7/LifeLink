#ifndef ELIGIBILITY_H
#define ELIGIBILITY_H

#include <array>
#include <string>
#include <unordered_map>

// Pre-donation questionnaire a donor must pass before accepting a request.
// Based on the NBTC (National Blood Transfusion Council, India) donor selection
// criteria. It is only a self-check: the blood centre still tests haemoglobin,
// blood pressure and so on before the actual donation.
//
// Answers are checked and then discarded. Only the time the donor passed the
// questionnaire is stored, so no personal health answers are kept.

struct EligibilityQuestion {
    const char* id;
    const char* text;
    bool safeAnswer;       // the answer that allows donation
    const char* reason;    // shown when the other answer is given
};

inline const std::array<EligibilityQuestion, 14>& getEligibilityQuestions() {
    static const std::array<EligibilityQuestion, 14> questions = {{
        {"age", "Are you between 18 and 65 years old?", true,
         "Donors must be 18 to 65 years old."},
        {"weight", "Do you weigh at least 45 kg?", true,
         "Donors must weigh at least 45 kg."},
        {"feeling_well", "Are you feeling healthy today, with no fever, cold or cough?", true,
         "Please wait until you have fully recovered and feel well."},
        {"meal", "Have you eaten a meal in the last 4 hours?", true,
         "Please eat a meal before donating; donating on an empty stomach is not allowed."},
        {"alcohol", "Have you had any alcohol in the last 24 hours?", false,
         "Please do not donate within 24 hours of drinking alcohol."},
        {"antibiotics", "Have you taken antibiotics in the last 2 weeks?", false,
         "Please wait 2 weeks after finishing antibiotics."},
        {"tattoo", "Have you had a tattoo or body piercing in the last 12 months?", false,
         "Please wait 12 months after a tattoo or piercing."},
        {"surgery", "Have you had major surgery in the last 12 months, or minor surgery in the last 3 months?", false,
         "Please wait 12 months after major surgery or 3 months after minor surgery."},
        {"transfusion", "Have you received a blood transfusion in the last 12 months?", false,
         "Please wait 12 months after receiving a blood transfusion."},
        {"malaria", "Have you had malaria in the last 3 months?", false,
         "Please wait 3 months after recovering from malaria."},
        {"dental", "Have you had a tooth extraction or root canal in the last 14 days?", false,
         "Please wait 14 days after a tooth extraction or root canal."},
        {"vaccination", "Have you had any vaccination in the last 28 days?", false,
         "Please wait 14 to 28 days after a vaccination, depending on the vaccine."},
        {"pregnancy", "Women only: are you pregnant, or have you given birth, breastfed or had an abortion "
                      "in the last 12 months? (Answer No if this does not apply.)", false,
         "Please wait 12 months after delivery or breastfeeding, or 6 months after an abortion."},
        {"permanent_conditions", "Have you ever had hepatitis B or C, HIV, cancer, heart disease "
                                 "or a bleeding disorder?", false,
         "People with these conditions cannot donate blood."},
    }};
    return questions;
}

struct EligibilityResult {
    bool eligible = true;
    std::string missingQuestionId;           // set when a question was not answered
    std::array<const char*, 14> reasons{};   // reasons for each failed question
    std::size_t reasonCount = 0;
};

// answers: question id -> the donor's yes (true) / no (false) answer.
inline EligibilityResult evaluateEligibility(const std::unordered_map<std::string, bool>& answers) {
    EligibilityResult result;
    for (const EligibilityQuestion& question : getEligibilityQuestions()) {
        const auto answer = answers.find(question.id);
        if (answer == answers.end()) {
            result.eligible = false;
            result.missingQuestionId = question.id;
            return result;
        }
        if (answer->second != question.safeAnswer) {
            result.eligible = false;
            result.reasons[result.reasonCount++] = question.reason;
        }
    }
    return result;
}

#endif
