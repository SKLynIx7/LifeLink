cmake_minimum_required(VERSION 3.15)
project(LifeLink)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(Crow CONFIG REQUIRED)
find_package(nlohmann_json CONFIG REQUIRED)
find_package(unofficial-sqlite3 CONFIG REQUIRED)

add_executable(LifeLink
    main.cpp
    engine/Donor.cpp
    engine/MatchRequest.cpp
    db/Database.cpp
    server/PasswordHasher.cpp
    server/Routes.cpp
)
target_link_libraries(LifeLink PRIVATE
    Crow::Crow
    nlohmann_json::nlohmann_json
    unofficial::sqlite3::sqlite3
    bcrypt
)
