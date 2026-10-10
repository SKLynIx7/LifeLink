#include "crow.h"
#include <nlohmann/json.hpp>
#include <sqlite3.h>

#include <iostream>
#include <string>

#include "engine/MatchRequest.h"
#include "db/Database.h"
#include "server/Routes.h"

void runEngineDemo() {
    MatchRequest request(101, 1, "Kiran", "A+", 1);
    request.addCandidate(Donor(1, "Anjali", "9876543210", "O-", 4.2, 120, 4.8));
    request.addCandidate(Donor(2, "Rahul", "9876543211", "A+", 2.5, 140, 4.4));
    request.addCandidate(Donor(3, "Meera", "9876543212", "O+", 1.8, 45, 4.9));
    request.addCandidate(Donor(4, "Vikram", "9876543213", "B+", 3.1, 200, 4.7));

    std::cout << "Eligible candidates for A+: " << request.getEligibleCandidateCount() << std::endl;
    const std::optional<Donor> firstCandidate = request.getHighestRankedDonor();

    if (firstCandidate.has_value()) {
        std::cout << "First donor to contact: " << firstCandidate->getName() << std::endl;
        request.recordResponse(firstCandidate->getId(), false);
    }

    const std::optional<Donor> nextCandidate = request.getHighestRankedDonor();
    if (nextCandidate.has_value()) {
        std::cout << "Next donor after decline: " << nextCandidate->getName() << std::endl;
    }

    MatchRequest balancedRequest(102, 1, "Kiran", "A+", 1);
    balancedRequest.addCandidate(Donor(5, "Reliable donor", "9876543214", "A+", 5.0, 120, 4.5));
    balancedRequest.addCandidate(Donor(6, "Nearby donor", "9876543215", "A+", 2.0, 120, 3.1));
    balancedRequest.addCandidate(Donor(7, "Low-rated donor", "9876543216", "A+", 1.0, 120, 2.9));

    const std::optional<Donor> balancedTopDonor = balancedRequest.getHighestRankedDonor();
    if (balancedTopDonor.has_value()) {
        std::cout << "Balanced ranking selects: " << balancedTopDonor->getName() << std::endl;
    }
}

void runDatabaseDemo() {
    Database database(":memory:");
    if (!database.open() || !database.initialize()) {
        std::cout << "Database setup failed: " << database.getLastError() << std::endl;
        return;
    }

    const std::optional<int> receiverId = database.registerUser(
        "Kiran", "kiran@example.com", "temporary-hash", "receiver",
        "What is your birthplace?", "Bengaluru");
    const std::optional<int> firstDonorId = database.registerUser(
        "Anjali", "anjali@example.com", "temporary-hash", "donor",
        "What is your birthplace?", "Bengaluru");
    const std::optional<int> secondDonorId = database.registerUser(
        "Rahul", "rahul@example.com", "temporary-hash", "donor",
        "What is your birthplace?", "Bengaluru");
    const std::optional<int> lowRatedDonorId = database.registerUser(
        "Sanjay", "sanjay@example.com", "temporary-hash", "donor",
        "What is your birthplace?", "Bengaluru");

    if (!receiverId.has_value() || !firstDonorId.has_value() || !secondDonorId.has_value() ||
        !lowRatedDonorId.has_value()) {
        std::cout << "Demo account setup failed: " << database.getLastError() << std::endl;
        return;
    }

    database.createDonorProfile(*firstDonorId, "9876543210", "O-", 12.9700, 77.5900);
    database.createDonorProfile(*secondDonorId, "9876543211", "A+", 12.9750, 77.6000);
    database.createDonorProfile(*lowRatedDonorId, "9876543212", "A+", 12.9896, 77.5946);
    database.updateDonorReliability(*lowRatedDonorId, 2.9);
    const std::optional<int> requestId = database.createBloodRequest(*receiverId, 1, "A+", 1);

    if (!requestId.has_value()) {
        std::cout << "Blood request creation failed: " << database.getLastError() << std::endl;
        return;
    }

    MatchRequest request(*requestId, 1, "Kiran", "A+", 1);
    const int candidateCount = database.loadCandidatesForRequest(request);
    std::cout << "SQLite loaded " << candidateCount << " eligible candidates." << std::endl;

    const std::optional<Donor> topDonor = request.getHighestRankedDonor();
    if (topDonor.has_value()) {
        std::cout << "Top donor from the min-heap: " << topDonor->getName() << std::endl;
    }
    std::cout << "Nearby lower-rated donors shown separately: "
              << database.getNearbyLowerRatedDonors(request).size() << std::endl;
}

int main(int argumentCount, char* arguments[]) {
    if (argumentCount > 1 && std::string(arguments[1]) == "--engine-demo") {
        runEngineDemo();
        return 0;
    }
    if (argumentCount > 1 && std::string(arguments[1]) == "--database-demo") {
        runDatabaseDemo();
        return 0;
    }

    nlohmann::json testJson;
    testJson["library"] = "nlohmann-json";
    testJson["working"] = true;
    std::cout << "JSON test: " << testJson.dump() << std::endl;

    sqlite3* testDatabase;
    const int result = sqlite3_open(":memory:", &testDatabase);
    if (result == SQLITE_OK) {
        std::cout << "SQLite test: database opened successfully" << std::endl;
        sqlite3_close(testDatabase);
    } else {
        std::cout << "SQLite test: failed to open database" << std::endl;
    }

    Database database("lifelink.db");
    if (!database.open() || !database.initialize()) {
        std::cerr << "Database setup failed: " << database.getLastError() << std::endl;
        return 1;
    }

    crow::SimpleApp app;
    registerRoutes(app, database);
    app.bindaddr("127.0.0.1").port(8080).run();

    return 0;
}
