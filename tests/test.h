// Dependency-free test harness: TEST(name) registers a function, CHECK*
// record failures without aborting the test binary.
#pragma once
#include <cmath>
#include <cstdio>
#include <functional>
#include <string>
#include <vector>

namespace bll::test {

struct Case {
    const char* name;
    void (*fn)();
};

inline std::vector<Case>& registry() {
    static std::vector<Case> cases;
    return cases;
}

inline int& failures() {
    static int n = 0;
    return n;
}

struct Registrar {
    Registrar(const char* name, void (*fn)()) { registry().push_back({name, fn}); }
};

inline void fail(const char* file, int line, const std::string& what) {
    ++failures();
    std::printf("    FAIL %s:%d: %s\n", file, line, what.c_str());
}

} // namespace bll::test

#define TEST(name)                                                        \
    static void test_##name();                                            \
    static ::bll::test::Registrar registrar_##name(#name, &test_##name);  \
    static void test_##name()

#define CHECK(cond)                                                       \
    do {                                                                  \
        if (!(cond)) ::bll::test::fail(__FILE__, __LINE__, #cond);        \
    } while (0)

#define CHECK_EQ(a, b)                                                    \
    do {                                                                  \
        const auto va_ = (a);                                             \
        const auto vb_ = (b);                                             \
        if (!(va_ == vb_))                                                \
            ::bll::test::fail(__FILE__, __LINE__,                         \
                              std::string(#a " == " #b " (") + std::to_string(va_) + " vs " + std::to_string(vb_) + ")"); \
    } while (0)

#define CHECK_NEAR(a, b, tol)                                             \
    do {                                                                  \
        const double va_ = static_cast<double>(a);                        \
        const double vb_ = static_cast<double>(b);                        \
        if (std::fabs(va_ - vb_) > static_cast<double>(tol))              \
            ::bll::test::fail(__FILE__, __LINE__,                         \
                              std::string(#a " ~= " #b " (") + std::to_string(va_) + " vs " + std::to_string(vb_) + ")"); \
    } while (0)
