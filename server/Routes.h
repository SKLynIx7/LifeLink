#ifndef ROUTES_H
#define ROUTES_H

#include "crow.h"

class Database;

void registerRoutes(crow::SimpleApp& app, Database& database);

#endif
