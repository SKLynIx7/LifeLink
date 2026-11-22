#ifndef TEST_FRAMEWORK_H
#define TEST_FRAMEWORK_H

// A very small test runner, so the tests need no extra library.
// TEST(name) { ... } defines a test; CHECK(condition) records a failure without stopping.

#include <cmath>
#include <functional>
#include <iostream>
#include <string>
#include <vector>

namespace testing {

struct TestCase {
    std::string name;
    std::function<void()> body;
};

inline std::vector<TestCase>& registry() {
    static std::vector<TestCase> tests;
    return tests;
}

inline int& failureCount() {
    static int failures = 0;
    return failures;
}

struct Registrar {
    Registrar(const std::string& name, std::function<void()> body) {
        registry().push_back({name, std::move(body)});
    }
};

inline void reportFailure(const char* file, int line, const std::string& what) {
    ++failureCount();
    std::cout << "    FAILED at " << file << ':' << line << ": " << what << '\n';
}

inline bool nearlyEqual(double first, double second, double tolerance = 1e-9) {
    return std::fabs(first - second) <= tolerance;
}

}  // namespace testing

#define TEST_CONCAT_INNER(a, b) a##b
#define TEST_CONCAT(a, b) TEST_CONCAT_INNER(a, b)

#define TEST(name)                                                                      \
    static void TEST_CONCAT(test_, name)();                                             \
    static const testing::Registrar TEST_CONCAT(registrar_, name)(#name, TEST_CONCAT(test_, name)); \
    static void TEST_CONCAT(test_, name)()

#define CHECK(condition)                                                                \
    do {                                                                                \
        if (!(condition)) {                                                             \
            testing::reportFailure(__FILE__, __LINE__, #condition);                     \
        }                                                                               \
    } while (false)

#define CHECK_NEAR(actual, expected)                                                    \
    do {                                                                                \
        if (!testing::nearlyEqual((actual), (expected), 1e-6)) {                        \
            testing::reportFailure(__FILE__, __LINE__,                                  \
                std::string(#actual) + " = " + std::to_string(actual) +                 \
                ", expected " + std::to_string(expected));                              \
        }                                                                               \
    } while (false)

#endif
