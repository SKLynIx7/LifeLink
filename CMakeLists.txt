cmake_minimum_required(VERSION 3.15)
project(LifeLink LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT WIN32)
    message(FATAL_ERROR "LifeLink uses the Windows BCrypt API for password hashing. Build it on Windows.")
endif()

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

# Copy the frontend next to the executable so the server finds it from build/Debug too.
add_custom_command(TARGET LifeLink POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
            ${CMAKE_SOURCE_DIR}/web $<TARGET_FILE_DIR:LifeLink>/web
)
