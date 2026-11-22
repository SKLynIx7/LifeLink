// Demo data for presentations: run "LifeLink --seed-demo --db demo.db", then start the
// server with "--db demo.db". Everything is created through the normal Database functions,
// so it follows the same rules as data entered on the website.

#include "DemoData.h"

#include "Database.h"
#include "../server/PasswordHasher.h"
#include "../server/Validation.h"

#include <ctime>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <vector>

namespace {
const char* QUESTION = "What is your favourite food?";
const char* ANSWER = "demo";  // security answer for every demo account

struct Person {
    const char* name;
    const char* email;      // without the domain
    const char* bloodType;
    const char* area;
    double latitude;
    double longitude;
    const char* phone;      // empty: not a donor
    const char* note;       // what this account shows in the demo
};

// Distances are from Doon Hospital (Parade Ground), where most demo requests are made.
const std::vector<Person> PEOPLE = {
    // Receivers and the administrator (no donor profile).
    {"Anita Sharma", "admin", "O+", "Clock Tower / Paltan Bazaar", 30.3243, 78.0414, "", "administrator: approves hospitals"},
    {"Riya Verma", "riya", "A+", "Dalanwala", 30.3170, 78.0560, "", "receiver: make a new request live; has a past request rated 4 stars"},
    {"Karan Mehta", "karan", "B+", "Rajpur Road", 30.3456, 78.0617, "", "receiver: open A+ request at Doon Hospital, waiting for Asha"},
    {"Meena Bhatt", "meena", "O+", "Patel Nagar", 30.3050, 78.0200, "", "receiver: used for the rating history"},
    // Donors.
    {"Asha Rawat", "asha", "A+", "Clock Tower / Paltan Bazaar", 30.3243, 78.0414, "9876500001", "donor: has an invitation to accept (questionnaire), sees urgent needs"},
    {"Rahul Negi", "rahul", "A+", "Ballupur", 30.3335, 78.0110, "9876500002", "donor: rated 4 and 5 stars"},
    {"Vikram Bisht", "vikram", "O+", "Rajpur Road", 30.3456, 78.0617, "9876500003", "donor: rated 4 stars, sees the Critical O+ post"},
    {"Meera Joshi", "meera", "O+", "Patel Nagar", 30.3050, 78.0200, "9876500004", "donor: gave blood 30 days ago, so not listed (90-day rule)"},
    {"Sanjay Thapa", "sanjay", "A+", "Vasant Vihar", 30.3260, 78.0060, "9876500005", "donor: rated 1 and 2 stars, shows 'Low reliability'"},
    {"Pooja Rana", "pooja", "B+", "Jakhan", 30.3620, 78.0700, "9876500006", "donor"},
    {"Deepak Kumar", "deepak", "AB+", "Sahastradhara Road", 30.3420, 78.0880, "9876500007", "donor: switched to not available"},
    {"Neha Gupta", "neha", "A-", "Raipur", 30.3100, 78.0900, "9876500008", "donor"},
    {"Arjun Singh", "arjun", "O+", "Clement Town", 30.2690, 78.0090, "9876500009", "donor"},
    {"Isha Kapoor", "isha", "AB-", "Prem Nagar", 30.3340, 77.9600, "9876500010", "donor: about 8 km away"},
    {"Kavya Pant", "kavya", "B-", "Doiwala", 30.1760, 78.1240, "9876500011", "donor: about 18 km away; a B- request widens 5 -> 20 km to find her"},
    {"Farhan Ali", "farhan", "A+", "Raipur", 30.3100, 78.0900, "9876500012", "donor"},
    {"Rohit Chauhan", "rohit", "O-", "Rishikesh", 30.0869, 78.2676, "9876500013", "donor: about 36 km away, never listed (limit 25 km)"},
};

const char* DOMAIN = "@demo.lifelink";

std::string email(const char* name) {
    return std::string(name) + DOMAIN;
}

std::string daysAgo(int days) {
    std::time_t when = std::time(nullptr) - static_cast<std::time_t>(days) * 24 * 60 * 60;
    std::tm date{};
#ifdef _WIN32
    localtime_s(&date, &when);
#else
    localtime_r(&when, &date);
#endif
    char text[11];
    std::strftime(text, sizeof(text), "%Y-%m-%d", &date);
    return text;
}

// Stops the seeding with a message that names the step that failed.
template <typename T>
T must(const std::optional<T>& value, Database& database, const std::string& step) {
    if (!value) {
        throw std::runtime_error(step + ": " + database.getLastError());
    }
    return *value;
}

void must(bool ok, Database& database, const std::string& step) {
    if (!ok) {
        throw std::runtime_error(step + ": " + database.getLastError());
    }
}

int hospitalIdByName(Database& database, const std::string& name) {
    for (const HospitalRecord& hospital : database.listHospitals()) {
        if (hospital.name == name) {
            return hospital.id;
        }
    }
    throw std::runtime_error("listed hospital not found: " + name);
}

// A finished donation with a rating, made the same way the website makes it:
// request -> invitation -> accepted -> completed -> rated.
void pastDonation(Database& database, int receiverId, int donorId, int hospitalId,
                  const std::string& bloodType, int stars) {
    const int requestId = must(database.createBloodRequest(receiverId, hospitalId, bloodType, 1), database, "past request");
    const int invitationId = must(database.createInvitation(requestId, donorId), database, "past invitation");
    must(database.respondToInvitation(invitationId, true), database, "past acceptance");
    must(database.updateRequestStatus(requestId, RequestStatus::FULFILLED), database, "past fulfilment");
    must(database.completeRequest(requestId), database, "past completion");
    must(database.addRating(requestId, receiverId, donorId, stars), database, "past rating");
}
}  // namespace

bool seedDemoData(Database& database, std::string& report) {
    const int existing = database.countUsers();
    if (existing != 0) {
        report = existing < 0 ? "Could not read the database: " + database.getLastError()
                              : "This database already has accounts. Demo data only goes into a new, empty "
                                "database file, e.g.  LifeLink --seed-demo --db demo.db";
        return false;
    }

    const std::string passwordHash = PasswordHasher::hashPassword(DEMO_PASSWORD);
    const std::string answerHash = PasswordHasher::hashPassword(Validation::normalizeAnswer(ANSWER));
    if (passwordHash.empty() || answerHash.empty()) {
        report = "Could not hash the demo password.";
        return false;
    }

    // All or nothing: a failure part-way leaves the database empty.
    if (!database.beginTransaction()) {
        report = "Could not start a transaction: " + database.getLastError();
        return false;
    }
    try {
        // ---- People
        std::vector<int> ids;
        for (const Person& person : PEOPLE) {
            const UserLocation location{std::string(person.area) + ", Dehradun", person.latitude, person.longitude,
                                        std::string(person.area) + ", Dehradun, Uttarakhand, India",
                                        "Dehradun", "Uttarakhand", "India", "", ""};
            const int id = must(database.registerUser(person.name, email(person.email), passwordHash, QUESTION,
                                                      answerHash, person.bloodType, location),
                                database, std::string("account ") + person.email);
            if (*person.phone) {
                must(database.createDonorProfile(id, person.phone), database, std::string("donor ") + person.email);
            }
            ids.push_back(id);
        }
        auto idOf = [&](const char* name) {
            for (std::size_t index = 0; index < PEOPLE.size(); ++index) {
                if (std::string(PEOPLE[index].email) == name) {
                    return ids[index];
                }
            }
            throw std::runtime_error(std::string("unknown demo account ") + name);
        };
        must(database.setAdminByEmail(email("admin"), true), database, "administrator");

        // ---- Listed hospitals in different states
        const int doon = hospitalIdByName(database, "Doon Hospital");
        const int coronation = hospitalIdByName(database, "Coronation Hospital");
        const int synergy = hospitalIdByName(database, "Synergy Institute of Medical Sciences");
        const int cmi = hospitalIdByName(database, "CMI Hospital");
        const int maxHospital = hospitalIdByName(database, "Max Super Speciality Hospital");

        HospitalRecord regular = *database.getHospital(coronation);  // regular hours instead of 24 hours
        regular.phone = "0135-2722101";
        regular.openingTime = "09:00";
        regular.closingTime = "17:00";
        regular.workingDays = {"Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
        must(database.updateHospitalProfile(coronation, regular), database, "Coronation hours");
        // The countdown starts when the demo data is created: seed shortly before presenting.
        must(database.setHospitalAvailability(synergy, "closing_soon", 240, "Staff unavailability"), database, "Synergy closing soon");
        must(database.setHospitalAvailability(cmi, "temporarily_closed", 0, "Emergency maintenance"), database, "CMI closed");
        for (int id : {doon, maxHospital}) {
            HospitalRecord hospital = *database.getHospital(id);
            hospital.phone = id == doon ? "0135-2659355" : "0135-7193000";
            hospital.facilities = {"blood_bank", "emergency", "icu", "ambulance"};
            must(database.updateHospitalProfile(id, hospital), database, "listed hospital phone");
        }

        // ---- Hospital accounts: one approved blood bank, one waiting for approval
        HospitalRecord bloodBank;
        bloodBank.name = "Himalayan Blood Bank";
        bloodBank.email = "bloodbank@demo.lifelink";
        bloodBank.phone = "0135-2733000";
        bloodBank.emergencyPhone = "9876511111";
        bloodBank.address = "22 Rajpur Road, Jakhan";
        bloodBank.city = "Dehradun";
        bloodBank.state = "Uttarakhand";
        bloodBank.country = "India";
        bloodBank.pincode = "248001";
        bloodBank.latitude = 30.3560;
        bloodBank.longitude = 78.0660;
        bloodBank.hospitalType = "Trust";
        bloodBank.licenseNumber = "UK-BB-2024-118";
        bloodBank.bloodGroups = {"A+", "A-", "B+", "O+", "O-"};
        bloodBank.workingDays = {"Mon", "Tue", "Wed", "Thu", "Fri", "Sat", "Sun"};
        bloodBank.description = "Licensed blood bank open 24 hours, with component separation.";
        bloodBank.facilities = {"blood_bank", "laboratory"};
        const int bloodBankId = must(database.registerHospital(bloodBank, passwordHash), database, "blood bank");
        must(database.setHospitalVerification(bloodBankId, "verified", ""), database, "blood bank approval");
        must(database.createEmergencyPost(bloodBankId, "O+", 2, "critical",
                                          "Road accident, patient in ICU. Ask for the blood bank on the ground floor.", 24),
             database, "critical post");
        must(database.createEmergencyPost(bloodBankId, "A+", 3, "urgent", "Planned surgery tomorrow morning.", 24),
             database, "urgent post");

        HospitalRecord pending;
        pending.name = "Green Valley Hospital";
        pending.email = "greenvalley@demo.lifelink";
        pending.phone = "0135-2744000";
        pending.address = "Chakrata Road, Prem Nagar";
        pending.city = "Dehradun";
        pending.state = "Uttarakhand";
        pending.country = "India";
        pending.pincode = "248007";
        pending.latitude = 30.3345;
        pending.longitude = 77.9650;
        pending.hospitalType = "Private";
        pending.licenseNumber = "UK-HOSP-5521";
        pending.bloodGroups = {"A+", "B+", "O+"};
        pending.openingTime = "08:00";
        pending.closingTime = "22:00";
        pending.workingDays = {"Mon", "Tue", "Wed", "Thu", "Fri", "Sat", "Sun"};
        pending.facilities = {"emergency", "pharmacy"};
        must(database.registerHospital(pending, passwordHash), database, "pending hospital");  // stays pending

        // ---- Rating history (each a real completed donation)
        pastDonation(database, idOf("riya"), idOf("vikram"), doon, "O+", 4);
        pastDonation(database, idOf("meena"), idOf("rahul"), maxHospital, "A+", 4);
        pastDonation(database, idOf("karan"), idOf("rahul"), doon, "A+", 5);
        pastDonation(database, idOf("meena"), idOf("sanjay"), doon, "A+", 1);
        pastDonation(database, idOf("karan"), idOf("sanjay"), maxHospital, "A+", 2);
        // Those donations were long ago, so these donors can give again today.
        for (const char* donor : {"vikram", "rahul", "sanjay"}) {
            must(database.setLastDonationDate(idOf(donor), daysAgo(150)), database, "old donation date");
        }
        must(database.setLastDonationDate(idOf("meera"), daysAgo(30)), database, "recent donation");
        must(database.setDonorAvailability(idOf("deepak"), false), database, "unavailable donor");

        // ---- An open request with an invitation waiting (expires 60 minutes after seeding)
        const int openRequest = must(database.createBloodRequest(idOf("karan"), doon, "A+", 1), database, "open request");
        must(database.createInvitation(openRequest, idOf("asha")), database, "waiting invitation");

        if (!database.commit()) {
            throw std::runtime_error("commit: " + database.getLastError());
        }
    } catch (const std::exception& error) {
        database.rollback();
        report = std::string("Demo data was not created (") + error.what() + ").";
        return false;
    }

    std::ostringstream out;
    out << "Demo data created. Every account's password is " << DEMO_PASSWORD
        << " (security answer: " << ANSWER << ").\n\n"
        << "People (log in at http://127.0.0.1:8080):\n";
    for (const Person& person : PEOPLE) {
        out << "  " << email(person.email) << "  " << person.bloodType << ", " << person.area << "  - " << person.note << '\n';
    }
    out << "\nHospitals (log in at http://127.0.0.1:8080/hospital-login.html):\n"
        << "  bloodbank@demo.lifelink    Himalayan Blood Bank: approved, has a Critical O+ and an Urgent A+ post\n"
        << "  greenvalley@demo.lifelink  Green Valley Hospital: waiting for the administrator's approval\n"
        << "\nHospital states: Synergy closes in 4 hours (Closing soon), CMI is temporarily closed,\n"
        << "Coronation is open 09:00-17:00 Mon-Sat (shown Closed outside those hours), the others 24 hours.\n"
        << "Time-based parts (Asha's invitation: 60 min, Synergy: 4 h) start now, so create the demo\n"
        << "data shortly before presenting. Run the server with the same --db option.\n";
    report = out.str();
    return true;
}
