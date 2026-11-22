#include "crow.h"

#include <cctype>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <optional>
#include <string>

#include "db/Database.h"
#include "db/DemoData.h"
#include "engine/MatchRequest.h"
#include "engine/RadiusSearch.h"
#include "server/Routes.h"

namespace {
struct ServerConfig {
    std::string databasePath = "lifelink.db";
    std::string webRoot;  // empty = find it automatically
    int port = 8080;
};

void printUsage() {
    std::cout << "Usage: LifeLink [--port 8080] [--db lifelink.db] [--web-dir path/to/web]\n"
              << "       LifeLink --seed-demo --db demo.db   (fill a NEW database with demo data, then exit)\n"
              << "       LifeLink --create-admin <email> [--db lifelink.db]   (make an account an administrator)\n"
              << "       LifeLink --remove-admin <email> [--db lifelink.db]\n"
              << "       LifeLink --engine-demo\n"
              << "       LifeLink --database-demo\n";
}

// Finds the web folder whether the exe runs from the project root or from build/Debug.
std::optional<std::string> findWebRoot(const std::string& configured, const char* executablePath) {
    namespace fs = std::filesystem;
    if (!configured.empty()) {
        return fs::exists(fs::path(configured) / "index.html")
                   ? std::optional<std::string>(configured) : std::nullopt;
    }

    const fs::path executableDirectory = fs::absolute(executablePath).parent_path();
    const fs::path candidates[] = {
        fs::current_path() / "web",
        executableDirectory / "web",
        executableDirectory.parent_path() / "web",
        executableDirectory.parent_path().parent_path() / "web",
    };
    for (const fs::path& candidate : candidates) {
        if (fs::exists(candidate / "index.html")) {
            return candidate.string();
        }
    }
    return std::nullopt;
}

void runEngineDemo() {
    MatchRequest request(101, 1, "A+");
    request.addCandidate(Donor(1, "Anjali", "9876543210", "O-", 4.2, 120, 4.8));
    request.addCandidate(Donor(2, "Rahul", "9876543211", "A+", 2.5, 140, 4.4));
    request.addCandidate(Donor(3, "Meera", "9876543212", "O+", 1.8, 45, 4.9));    // donated 45 days ago
    request.addCandidate(Donor(4, "Vikram", "9876543213", "B+", 3.1, 200, 4.7));  // incompatible
    request.addCandidate(Donor(5, "Farhan", "9876543214", "A+", 35.0, 200, 5.0)); // outside 25 km

    std::cout << "Eligible candidates for A+: " << request.getEligibleCandidateCount() << '\n';
    std::cout << "Ranked list shown to the receiver:\n";
    int rank = 0;
    for (const Donor& donor : request.getRankedDonors()) {
        std::cout << "  #" << ++rank << ' ' << donor.getName() << " (score "
                  << calculatePriorityScore(donor) << ")\n";
    }

    MatchRequest balancedRequest(102, 1, "A+");
    balancedRequest.addCandidate(Donor(6, "Reliable donor", "9876543215", "A+", 5.0, 120, 4.5));
    balancedRequest.addCandidate(Donor(7, "Nearby donor", "9876543216", "A+", 2.0, 120, 3.1));
    balancedRequest.addCandidate(Donor(8, "Low-rated donor", "9876543217", "A+", 1.0, 120, 2.9));

    const std::optional<Donor> balancedTopDonor = balancedRequest.getHighestRankedDonor();
    if (balancedTopDonor) {
        std::cout << "Balanced ranking selects: " << balancedTopDonor->getName() << '\n';
    }

    // "Invite top 2" pops the two best donors off the heap.
    MatchRequest inviteTop(103, 1, "AB+");
    inviteTop.addCandidate(Donor(9, "Asha", "9876543218", "AB-", 2.0, 120, 4.0));
    inviteTop.addCandidate(Donor(10, "Dev", "9876543219", "O+", 3.0, 120, 4.0));
    inviteTop.addCandidate(Donor(11, "Ira", "9876543220", "B+", 9.0, 120, 3.5));
    std::cout << "Invite top 2 for AB+:";
    for (int count = 0; count < 2; ++count) {
        const std::optional<Donor> best = inviteTop.popHighestRankedDonor();
        if (best) {
            std::cout << ' ' << best->getName();
        }
    }
    std::cout << " (" << inviteTop.getEligibleCandidateCount() << " left in the heap)\n";

    // Auto-expand: nobody within 5 or 10 km, so the search widens to 15 km and stops there.
    const RadiusSearchResult expanded = searchWithAutoExpand(104, 1, "O+", {
        Donor(12, "Neha", "9876543221", "O-", 12.0, 150, 4.6),
        Donor(13, "Arjun", "9876543222", "O+", 22.0, 150, 4.9),
    });
    std::cout << "Auto-expand for O+: tried";
    for (double radiusKm : expanded.radiiTried) {
        std::cout << ' ' << radiusKm << " km";
    }
    std::cout << " -> " << expanded.match.getEligibleCandidateCount() << " donor within "
              << expanded.match.getSearchRadiusKm() << " km\n";
}

void runDatabaseDemo() {
    Database database(":memory:");
    if (!database.open() || !database.initialize()) {
        std::cout << "Database setup failed: " << database.getLastError() << '\n';
        return;
    }

    // Demo accounts use placeholder hashes; they are never used to log in.
    const std::optional<int> receiverId = database.registerUser(
        "Kiran", "kiran@example.com", "demo-hash", "What is your birthplace?", "demo-hash",
        "A+", UserLocation{"Rajpur Road", 30.3456, 78.0617});
    const std::optional<int> firstDonorId = database.registerUser(
        "Anjali", "anjali@example.com", "demo-hash", "What is your birthplace?", "demo-hash",
        "O-", UserLocation{"Clock Tower", 30.3243, 78.0414});
    const std::optional<int> secondDonorId = database.registerUser(
        "Rahul", "rahul@example.com", "demo-hash", "What is your birthplace?", "demo-hash",
        "A+", UserLocation{"Dalanwala", 30.3170, 78.0560});
    const std::optional<int> lowRatedDonorId = database.registerUser(
        "Sanjay", "sanjay@example.com", "demo-hash", "What is your birthplace?", "demo-hash",
        "A+", UserLocation{"Ballupur", 30.3335, 78.0110});

    if (!receiverId || !firstDonorId || !secondDonorId || !lowRatedDonorId) {
        std::cout << "Demo account setup failed: " << database.getLastError() << '\n';
        return;
    }

    database.createDonorProfile(*firstDonorId, "9876543210");
    database.createDonorProfile(*secondDonorId, "9876543211");
    database.createDonorProfile(*lowRatedDonorId, "9876543212");
    database.updateDonorReliability(*lowRatedDonorId, 2.9);

    const std::optional<int> requestId = database.createBloodRequest(*receiverId, 1, "A+", 1);
    if (!requestId) {
        std::cout << "Blood request creation failed: " << database.getLastError() << '\n';
        return;
    }

    MatchRequest request(*requestId, 1, "A+");
    const int candidateCount = database.loadCandidatesForRequest(request, *receiverId);
    std::cout << "SQLite loaded " << candidateCount << " eligible candidates.\n";

    const std::optional<Donor> topDonor = request.getHighestRankedDonor();
    if (topDonor) {
        std::cout << "Top donor from the priority queue: " << topDonor->getName() << " ("
                  << topDonor->getDistanceKm() << " km)\n";
    }
}
}

// --create-admin / --remove-admin: changes an existing account, then exits (the server does not start).
int changeAdmin(const std::string& databasePath, const std::string& rawEmail, bool makeAdmin) {
    std::string email = rawEmail;
    for (char& character : email) {
        character = static_cast<char>(std::tolower(static_cast<unsigned char>(character)));
    }
    Database database(databasePath);
    if (!database.open() || !database.initialize()) {
        std::cerr << "Database setup failed: " << database.getLastError() << '\n';
        return 1;
    }
    if (!database.setAdminByEmail(email, makeAdmin)) {
        std::cerr << database.getLastError()
                  << " Register that account on the website first, then run this command again.\n";
        return 1;
    }
    std::cout << (makeAdmin ? "Done: " + email + " is now an administrator. Log in with it and open "
                              "\"Approve hospitals\" from the menu under your name.\n"
                            : "Done: " + email + " is no longer an administrator.\n");
    return 0;
}

int main(int argumentCount, char* arguments[]) {
    ServerConfig config;
    std::string adminEmail;
    bool makeAdmin = true;
    bool seedDemo = false;

    for (int index = 1; index < argumentCount; ++index) {
        const std::string argument = arguments[index];
        const bool hasValue = index + 1 < argumentCount;

        if (argument == "--engine-demo") {
            runEngineDemo();
            return 0;
        }
        if (argument == "--database-demo") {
            runDatabaseDemo();
            return 0;
        }
        if (argument == "--help") {
            printUsage();
            return 0;
        }
        if (argument == "--seed-demo") {
            seedDemo = true;
            continue;
        }
        if ((argument == "--create-admin" || argument == "--remove-admin") && hasValue) {
            makeAdmin = argument == "--create-admin";
            adminEmail = arguments[++index];
            continue;
        }
        if (argument == "--port" && hasValue) {
            try {
                config.port = std::stoi(arguments[++index]);
            } catch (...) {
                config.port = 0;
            }
            if (config.port < 1 || config.port > 65535) {
                std::cerr << "Port must be a number between 1 and 65535.\n";
                return 1;
            }
        } else if (argument == "--db" && hasValue) {
            config.databasePath = arguments[++index];
        } else if (argument == "--web-dir" && hasValue) {
            config.webRoot = arguments[++index];
        } else {
            std::cerr << "Unknown option: " << argument << '\n';
            printUsage();
            return 1;
        }
    }

    if (!adminEmail.empty()) {
        return changeAdmin(config.databasePath, adminEmail, makeAdmin);
    }
    if (seedDemo) {
        Database database(config.databasePath);
        if (!database.open() || !database.initialize()) {
            std::cerr << "Database setup failed: " << database.getLastError() << '\n';
            return 1;
        }
        std::string report;
        const bool seeded = seedDemoData(database, report);
        (seeded ? std::cout : std::cerr) << report << '\n';
        if (seeded) {
            std::cout << "Start the server with:  LifeLink --db " << config.databasePath << '\n';
        }
        return seeded ? 0 : 1;
    }

    const std::optional<std::string> webRoot = findWebRoot(config.webRoot, arguments[0]);
    if (!webRoot) {
        std::cerr << "Could not find the web folder (index.html). Use --web-dir to point to it.\n";
        return 1;
    }

    Database database(config.databasePath);
    if (!database.open() || !database.initialize()) {
        std::cerr << "Database setup failed: " << database.getLastError() << '\n';
        return 1;
    }
    database.deleteExpiredSessions();

    crow::SimpleApp app;
    registerRoutes(app, database, *webRoot);

    std::cout << "LifeLink is running at http://127.0.0.1:" << config.port << '\n';

    // One worker thread: all handlers share one SQLite connection and use
    // transactions, so requests must not run in parallel on it.
    app.bindaddr("127.0.0.1").port(static_cast<std::uint16_t>(config.port)).concurrency(1).run();
    return 0;
}
