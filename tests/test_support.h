// Minimal assertion helpers for phi-core unit tests (no framework).
#pragma once

#include "phi/runtime/loop.h"

#include <chrono>
#include <cstdio>
#include <functional>

namespace phitest {

inline int g_failures = 0;

/// Runs the current thread's runtime loop in slices until `predicate` holds
/// or `timeoutMs` passes - what processEvents-pumping was before the 6c
/// thread architecture put everything on the loop.
inline bool waitFor(const std::function<bool()> &predicate, int timeoutMs = 5000)
{
    auto *loop = phi::runtime::Loop::current();
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    while (!predicate()) {
        if (std::chrono::steady_clock::now() >= deadline)
            return predicate();
        auto slice = loop->timerAfter(std::chrono::milliseconds(20),
                                      [loop]() { loop->stop(); });
        loop->run();
    }
    return true;
}

#define CHECK(cond)                                                                             \
    do {                                                                                        \
        if (!(cond)) {                                                                          \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                         \
            ++::phitest::g_failures;                                                            \
        }                                                                                       \
    } while (false)

#define CHECK_MSG(cond, ...)                                                                    \
    do {                                                                                        \
        if (!(cond)) {                                                                          \
            std::printf("FAIL %s:%d: %s (", __FILE__, __LINE__, #cond);                         \
            std::printf(__VA_ARGS__);                                                           \
            std::printf(")\n");                                                                 \
            ++::phitest::g_failures;                                                            \
        }                                                                                       \
    } while (false)

inline int report(const char *name)
{
    if (g_failures == 0) {
        std::printf("%s: all passed\n", name);
        return 0;
    }
    std::printf("%s: %d failure(s)\n", name, g_failures);
    return 1;
}

} // namespace phitest
