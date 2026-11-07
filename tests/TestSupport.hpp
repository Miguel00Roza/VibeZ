#pragma once

#include <cstdlib>
#include <iostream>
#include <string_view>

namespace vibez::test {

inline int failures = 0;

inline void Fail(const char* expression, const char* file, const int line, const std::string_view detail = {}) {
    ++failures;
    std::cerr << file << ':' << line << ": check failed: " << expression;
    if (!detail.empty()) std::cerr << " (" << detail << ')';
    std::cerr << '\n';
}

inline int Finish(const std::string_view suite) {
    if (failures == 0) {
        std::cout << suite << " passed\n";
        return 0;
    }
    std::cerr << suite << " failed with " << failures << " failure(s)\n";
    return 1;
}

} // namespace vibez::test

#define CHECK(expression) \
    do { if (!(expression)) ::vibez::test::Fail(#expression, __FILE__, __LINE__); } while (false)

#define CHECK_MSG(expression, detail) \
    do { if (!(expression)) ::vibez::test::Fail(#expression, __FILE__, __LINE__, (detail)); } while (false)
