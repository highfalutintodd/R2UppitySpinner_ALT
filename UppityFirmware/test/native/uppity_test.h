// Minimal host-side test harness (no external deps). Adapted from Roam-A-Dome ALT's rad_test.h.
#pragma once
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <vector>

namespace ut {

struct Case { const char* name; std::function<void()> fn; };
inline std::vector<Case>& registry() { static std::vector<Case> r; return r; }
inline int gFailures = 0;
inline const char* gCurrent = "";
struct Registrar { Registrar(const char* name, std::function<void()> fn) { registry().push_back({name, fn}); } };

#define TEST(name)                                                       \
    static void test_##name();                                           \
    static ut::Registrar reg_##name(#name, test_##name);                 \
    static void test_##name()

#define CHECK(cond)                                                                          \
    do { if (!(cond)) { ++ut::gFailures;                                                     \
        std::printf("FAIL %s: %s (%s:%d)\n", ut::gCurrent, #cond, __FILE__, __LINE__); } } while (0)

#define CHECK_EQ(a, b)                                                                                   \
    do { auto va = (a); auto vb = (b); if (!(va == vb)) { ++ut::gFailures;                              \
        std::printf("FAIL %s: %s == %s (%lld != %lld) (%s:%d)\n", ut::gCurrent, #a, #b,                 \
                    (long long)va, (long long)vb, __FILE__, __LINE__); } } while (0)

#define CHECK_NEAR(a, b, tol)                                                                            \
    do { double va = (a), vb = (b); if (!(va - vb <= (tol) && vb - va <= (tol))) { ++ut::gFailures;      \
        std::printf("FAIL %s: %s ~ %s (%g vs %g, tol %g) (%s:%d)\n", ut::gCurrent, #a, #b, va, vb,      \
                    (double)(tol), __FILE__, __LINE__); } } while (0)

inline int runAll(int argc, char** argv) {
    const char* filter = argc > 1 ? argv[1] : nullptr;
    size_t ran = 0;
    for (auto& c : registry()) {
        if (filter && !std::strstr(c.name, filter)) continue;
        gCurrent = c.name; c.fn(); ++ran;
    }
    if (gFailures == 0) std::printf("OK: %zu tests passed\n", ran);
    else std::printf("FAILED: %d check(s) failed across %zu tests\n", gFailures, ran);
    return gFailures == 0 ? 0 : 1;
}

} // namespace ut
