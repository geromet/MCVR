#pragma once
// Minimal dependency-free test harness (deterministic: no randomness, no timing assumptions).
#include <cstdio>
#include <functional>
#include <string>
#include <vector>

namespace mt {

struct Case {
    const char *name;
    std::function<void()> fn;
};

inline std::vector<Case> &registry() {
    static std::vector<Case> cases;
    return cases;
}

inline int &failures() {
    static int n = 0;
    return n;
}

struct Registrar {
    Registrar(const char *name, std::function<void()> fn) { registry().push_back({name, std::move(fn)}); }
};

} // namespace mt

#define TEST(name)                                                                                                     \
    static void name();                                                                                                \
    static ::mt::Registrar registrar_##name(#name, name);                                                              \
    static void name()

#define CHECK(cond)                                                                                                    \
    do {                                                                                                               \
        if (!(cond)) {                                                                                                 \
            std::fprintf(stderr, "    %s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond);                          \
            ++::mt::failures();                                                                                        \
        }                                                                                                              \
    } while (0)

#define CHECK_EQ(a, b)                                                                                                 \
    do {                                                                                                               \
        auto va_ = (a);                                                                                                \
        auto vb_ = (b);                                                                                                \
        if (!(va_ == vb_)) {                                                                                           \
            std::fprintf(stderr, "    %s:%d: CHECK_EQ failed: %s == %s (%lld vs %lld)\n", __FILE__, __LINE__, #a, #b,  \
                         (long long)va_, (long long)vb_);                                                              \
            ++::mt::failures();                                                                                        \
        }                                                                                                              \
    } while (0)

#define CHECK_THROWS(expr, ExType)                                                                                     \
    do {                                                                                                               \
        bool caught_ = false;                                                                                          \
        try {                                                                                                          \
            (void)(expr);                                                                                              \
        } catch (const ExType &) { caught_ = true; }                                                                   \
        if (!caught_) {                                                                                                \
            std::fprintf(stderr, "    %s:%d: expected %s from: %s\n", __FILE__, __LINE__, #ExType, #expr);             \
            ++::mt::failures();                                                                                        \
        }                                                                                                              \
    } while (0)
