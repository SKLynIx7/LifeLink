#ifndef ROUTES_H
#define ROUTES_H

#include "crow.h"

#include <string>

class Database;

// webRoot is the folder that holds index.html, styles.css and app.js.
void registerRoutes(crow::SimpleApp& app, Database& database, const std::string& webRoot);

#endif
