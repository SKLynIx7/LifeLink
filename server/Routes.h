#ifndef ROUTES_H
#define ROUTES_H

#include "crow.h"

#include <string>

#include "../engine/HospitalStatus.h"

#include <nlohmann/json.hpp>

class Database;
struct HospitalRecord;
struct EmergencyPost;

// webRoot is the folder that holds index.html, styles.css and app.js.
void registerRoutes(crow::SimpleApp& app, Database& database, const std::string& webRoot);

// Hospital accounts and hospital lists (HospitalRoutes.cpp). Called by registerRoutes.
void registerHospitalRoutes(crow::SimpleApp& app, Database& database);
// Whether the hospital can take a new donor visit right now (decided on the server).
HospitalStatus currentHospitalStatus(const HospitalRecord& hospital);
std::string hospitalUnavailableMessage();
// Everything about a hospital except its password: for the hospital itself and administrators.
nlohmann::json hospitalFullJson(const HospitalRecord& hospital);
nlohmann::json emergencyPostJson(const EmergencyPost& post);

#endif
