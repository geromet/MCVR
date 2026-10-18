#include "common/singleton.hpp"

#include <array>
#include <atomic>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <string_view>
#include <thread>
#include <vector>

namespace {
void require(bool good, const char* msg) {
    if (!good) throw std::runtime_error(msg);
}

template <typename E, typename F>
void require_throws(F&& f, const char* msg) {
    try { f(); }
    catch (const E&) { return; }
    throw std::runtime_error(msg);
}

struct Uninitialized final : Singleton<Uninitialized> {
    friend class Singleton<Uninitialized>;
};
struct Argument final : Singleton<Argument> {
    explicit Argument(int v) : value(v) {}
    int value;
    friend class Singleton<Argument>;
};
struct DoubleInit final : Singleton<DoubleInit> {
    explicit DoubleInit(int v) : value(v) {}
    int value;
    friend class Singleton<DoubleInit>;
};
struct Retry final : Singleton<Retry> {
    explicit Retry(bool reject) { if (reject) throw std::runtime_error("constructor refused"); }
    friend class Singleton<Retry>;
};
struct Concurrent final : Singleton<Concurrent> {
    Concurrent() { ++constructed; }
    static inline std::atomic<int> constructed{0};
    friend class Singleton<Concurrent>;
};

void singleton_uninitialized() {
    require(!Uninitialized::is_initialized(), "initial state was initialized");
    require(Uninitialized::try_instance() == nullptr, "initial pointer was nonnull");
    require_throws<std::logic_error>(
        []{ Uninitialized::instance(); }, "instance() before init did not throw");
}
void singleton_init_argument() {
    Argument::init(42);
    require(Argument::is_initialized(), "init() did not initialize");
    require(Argument::instance().value == 42, "constructor argument was lost");
    require(&Argument::instance() == Argument::try_instance(), "instance identity changed");
}
void singleton_double_init() {
    DoubleInit::init(42);
    require_throws<std::logic_error>(
        []{ DoubleInit::init(7); }, "double init was accepted");
    require(DoubleInit::instance().value == 42, "double init replaced original value");
}
void singleton_failed_construction_retry() {
    require_throws<std::runtime_error>(
        []{ Retry::init(true); }, "failing constructor did not throw");
    require(!Retry::is_initialized(), "failed constructor initialized singleton");
    Retry::init(false);
    require(Retry::is_initialized(), "retry failed");
}
void singleton_concurrent_init() {
    constexpr int n = 8;
    std::atomic<bool> start{false};
    std::atomic<int> winners{0}, rejected{0};
    std::vector<std::thread> workers;
    for (int i = 0; i < n; ++i) workers.emplace_back([&] {
        while (!start.load(std::memory_order_acquire)) std::this_thread::yield();
        try { Concurrent::init(); ++winners; }
        catch (const std::logic_error&) { ++rejected; }
    });
    start.store(true, std::memory_order_release);
    for (auto& worker : workers) worker.join();
    require(Concurrent::constructed == 1, "constructor ran more than once");
    require(winners == 1, "expected one initializer");
    require(rejected == n - 1, "other initializers were not rejected");
}

struct TestCase { std::string_view name; void (*run)(); };
constexpr std::array<TestCase, 5> cases{{
    {"singleton_uninitialized", singleton_uninitialized},
    {"singleton_init_argument", singleton_init_argument},
    {"singleton_double_init", singleton_double_init},
    {"singleton_failed_construction_retry", singleton_failed_construction_retry},
    {"singleton_concurrent_init", singleton_concurrent_init},
}};

int run_one(const TestCase& test) {
    try { test.run(); std::cout << "PASS " << test.name << '\n'; return 0; }
    catch (const std::exception& e) { std::cerr << "FAIL " << test.name << ": " << e.what() << '\n'; return 1; }
    catch (...) { std::cerr << "FAIL " << test.name << ": unknown exception\n"; return 1; }
}
} // namespace

int main(int argc, char** argv) {
    // Reject ambiguous registry identities before any tests run.
    for (std::size_t i = 0; i < cases.size(); ++i) {
        if (cases[i].name.empty()) return 2;
        for (std::size_t j = 0; j < i; ++j)
            if (cases[i].name == cases[j].name) return 2;
    }
    if (argc == 2 && std::strcmp(argv[1], "--list") == 0) {
        for (const auto& test : cases) std::cout << test.name << '\n';
        return 0;
    }
    if (argc == 3 && std::strcmp(argv[1], "--run-exact") == 0) {
        for (const auto& test : cases)
            if (test.name == argv[2]) return run_one(test);
        std::cerr << "unknown exact test identity: " << argv[2] << '\n';
        return 2;
    }
    if (argc != 1) {
        std::cerr << "usage: mcvr_singleton_tests [--list | --run-exact NAME]\n";
        return 2;
    }
    int failed = 0;
    for (const auto& test : cases) failed += run_one(test);
    return failed == 0 ? 0 : 1;
}
