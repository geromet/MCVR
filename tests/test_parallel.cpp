#include "harness.hpp"

#include "core/util/parallel.hpp"

#include <atomic>
#include <cstdlib>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

using mcvr::parallelFor;
using mcvr::parallelThreadCount;

namespace {
constexpr const char *ENV = "MCVR_TEST_THREADS";
std::mutex envMutex;

uint32_t hw() { return std::max(1u, std::thread::hardware_concurrency()); }
uint32_t defaultThreads() { return std::min(8u, hw()); }

struct SavedEnv {
    bool present;
    std::string value;
};

SavedEnv saveEnv() {
    const char *value = std::getenv(ENV);
    return {value != nullptr, value != nullptr ? value : ""};
}

void restoreEnv(const SavedEnv &saved) {
    if (saved.present) setenv(ENV, saved.value.c_str(), 1);
    else unsetenv(ENV);
}

uint32_t withEnv(const char *value) {
    std::lock_guard<std::mutex> lock(envMutex);
    const SavedEnv saved = saveEnv();
    if (value) setenv(ENV, value, 1);
    else unsetenv(ENV);
    const uint32_t result = parallelThreadCount(ENV);
    restoreEnv(saved);
    return result;
}
} // namespace

TEST(parallel_thread_count_default_is_capped_at_8) {
    CHECK_EQ(withEnv(nullptr), defaultThreads());
    CHECK_EQ(withEnv(""), defaultThreads());
}

TEST(parallel_thread_count_env_override) {
    CHECK_EQ(withEnv("1"), 1u);
    CHECK_EQ(withEnv("2"), std::min(2u, hw()));
    CHECK_EQ(withEnv("0002"), std::min(2u, hw()));
    CHECK_EQ(withEnv("4294967295"), hw());
}

TEST(parallel_thread_count_invalid_env_falls_back_to_default) {
    for (const char *bad : {"abc", "0", "0000", "-3", "+2", " 2", "2 ", "2junk", "4294967296", "999999999999999999999999999999999999"}) {
        CHECK_EQ(withEnv(bad), defaultThreads());
    }
}

TEST(parallel_thread_count_fixture_restores_exact_prior_state) {
    {
        std::lock_guard<std::mutex> lock(envMutex);
        unsetenv(ENV);
    }
    CHECK_EQ(withEnv("2junk"), defaultThreads());
    {
        std::lock_guard<std::mutex> lock(envMutex);
        CHECK(std::getenv(ENV) == nullptr);
        setenv(ENV, "", 1);
    }
    CHECK_EQ(withEnv("2"), std::min(2u, hw()));
    {
        std::lock_guard<std::mutex> lock(envMutex);
        const char *value = std::getenv(ENV);
        CHECK(value != nullptr);
        CHECK_EQ(std::string(value), std::string(""));
        setenv(ENV, "17", 1);
    }
    CHECK_EQ(withEnv("1"), 1u);
    {
        std::lock_guard<std::mutex> lock(envMutex);
        CHECK_EQ(std::string(std::getenv(ENV)), std::string("17"));
        unsetenv(ENV);
    }
}

TEST(parallel_thread_count_unprotected_fixture_can_be_contaminated) {
    std::lock_guard<std::mutex> outerLock(envMutex);
    const SavedEnv saved = saveEnv();
    std::atomic<bool> firstSet{false};
    std::atomic<bool> secondSet{false};
    std::string firstObserved;

    std::thread first([&] {
        setenv(ENV, "1", 1);
        firstSet.store(true, std::memory_order_release);
        while (!secondSet.load(std::memory_order_acquire)) std::this_thread::yield();
        const char *observed = std::getenv(ENV);
        firstObserved = observed != nullptr ? observed : "<unset>";
    });
    std::thread second([&] {
        while (!firstSet.load(std::memory_order_acquire)) std::this_thread::yield();
        setenv(ENV, "2", 1);
        secondSet.store(true, std::memory_order_release);
    });
    first.join();
    second.join();

    CHECK_EQ(firstObserved, std::string("2"));
    restoreEnv(saved);
}

TEST(parallel_thread_count_fixture_serializes_full_environment_interval) {
    std::atomic<int> inside{0};
    std::atomic<int> maxInside{0};
    auto attempt = [&](const char *value) {
        std::lock_guard<std::mutex> lock(envMutex);
        const SavedEnv saved = saveEnv();
        setenv(ENV, value, 1);
        const int now = ++inside;
        int observed = maxInside.load();
        while (observed < now && !maxInside.compare_exchange_weak(observed, now)) {}
        CHECK_EQ(parallelThreadCount(ENV), std::min(static_cast<uint32_t>(value[0] - '0'), hw()));
        std::this_thread::yield();
        --inside;
        restoreEnv(saved);
    };
    std::thread first(attempt, "1");
    std::thread second(attempt, "2");
    first.join();
    second.join();
    CHECK_EQ(maxInside.load(), 1);
}

TEST(parallel_for_visits_each_index_exactly_once) {
    std::lock_guard<std::mutex> lock(envMutex);
    const SavedEnv saved = saveEnv();
    setenv(ENV, "4", 1);
    constexpr size_t N = 1000;
    std::vector<std::atomic<int>> hits(N);
    parallelFor(N, [&](size_t i) { hits[i]++; }, ENV);
    for (size_t i = 0; i < N; i++) CHECK_EQ(hits[i].load(), 1);
    restoreEnv(saved);
}

TEST(parallel_for_zero_count_never_calls) {
    int calls = 0;
    parallelFor(0, [&](size_t) { calls++; }, ENV);
    CHECK_EQ(calls, 0);
}

TEST(parallel_for_single_thread_runs_in_order_on_caller) {
    std::lock_guard<std::mutex> lock(envMutex);
    const SavedEnv saved = saveEnv();
    setenv(ENV, "1", 1);
    std::vector<size_t> order;
    auto caller = std::this_thread::get_id();
    bool sameThread = true;
    parallelFor(5, [&](size_t i) {
        order.push_back(i);
        sameThread &= std::this_thread::get_id() == caller;
    }, ENV);
    CHECK((order == std::vector<size_t>{0, 1, 2, 3, 4}));
    CHECK(sameThread);
    restoreEnv(saved);
}

TEST(parallel_for_propagates_exception_and_still_finishes_other_work) {
    std::lock_guard<std::mutex> lock(envMutex);
    const SavedEnv saved = saveEnv();
    setenv(ENV, "4", 1);
    constexpr size_t N = 200;
    std::atomic<int> done{0};
    CHECK_THROWS(parallelFor(N, [&](size_t i) {
        done++;
        if (i == 50) throw std::runtime_error("boom");
    }, ENV), std::runtime_error);
    CHECK_EQ(done.load(), (int)N);
    restoreEnv(saved);
}
