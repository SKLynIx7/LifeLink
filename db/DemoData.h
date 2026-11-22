#ifndef DEMO_DATA_H
#define DEMO_DATA_H

#include <string>

class Database;

// Every demo account uses this password.
constexpr const char* DEMO_PASSWORD = "demo1234";

// Fills an EMPTY database with a ready-made Dehradun demo: an administrator, receivers,
// donors at different distances and blood groups (with rating history), hospitals in every
// availability state, urgent blood posts and a hospital waiting for approval.
// Refuses a database that already has accounts, so real data is never mixed with demo data.
// On success `report` lists the accounts to log in with; on failure it says why.
bool seedDemoData(Database& database, std::string& report);

#endif
