#include "TestFramework.h"

// Runs every TEST(...) and returns 0 only if all of them passed (so CTest reports it).
int main() {
    int failedTests = 0;
    for (const testing::TestCase& test : testing::registry()) {
        const int failuresBefore = testing::failureCount();
        test.body();
        const bool passed = testing::failureCount() == failuresBefore;
        std::cout << (passed ? "[PASS] " : "[FAIL] ") << test.name << '\n';
        if (!passed) {
            ++failedTests;
        }
    }

    const int total = static_cast<int>(testing::registry().size());
    std::cout << '\n' << total - failedTests << " of " << total << " tests passed.\n";
    return failedTests == 0 ? 0 : 1;
}
