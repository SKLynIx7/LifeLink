cmake_minimum_required(VERSION 3.15)
project(LifeLink LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

enable_testing()

# Unit tests. The engine has no Windows-only code, so the tests build on any system.
add_executable(LifeLinkTests
    tests/TestMain.cpp
    tests/RadiusSearchTests.cpp
    tests/HospitalStatusTests.cpp
    engine/Donor.cpp
    engine/MatchRequest.cpp
    engine/RadiusSearch.cpp
    engine/HospitalStatus.cpp
)
add_test(NAME LifeLinkTests COMMAND LifeLinkTests)

if(NOT WIN32)
    message(WARNING "The LifeLink server uses the Windows BCrypt API, so only the unit tests are built here. "
                    "Build on Windows for the full program.")
    return()
endif()

find_package(Crow CONFIG REQUIRED)
find_package(nlohmann_json CONFIG REQUIRED)
find_package(unofficial-sqlite3 CONFIG REQUIRED)

add_executable(LifeLink
    main.cpp
    engine/Donor.cpp
    engine/MatchRequest.cpp
    engine/RadiusSearch.cpp
    engine/HospitalStatus.cpp
    db/Database.cpp
    db/HospitalStore.cpp
    db/DemoData.cpp
    server/PasswordHasher.cpp
    server/Routes.cpp
    server/HospitalRoutes.cpp
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
