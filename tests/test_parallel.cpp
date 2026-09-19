#include "harness.hpp"

#include "core/util/parallel.hpp"

#include <atomic>
#include <cstdlib>
#include <stdexcept>
#include <thread>
#include <vector>

using mcvr::parallelFor;
using mcvr::parallelThreadCount;

namespace {
constexpr const char *ENV = "MCVR_TEST_THREADS";
uint32_t hw() { return std::max(1u, std::thread::hardware_concurrency()); }
uint32_t withEnv(const char *value) {
    if (value) setenv(ENV, value, 1);
    else unsetenv(ENV);
    return parallelThreadCount(ENV);
}
} // namespace

TEST(parallel_thread_count_default_is_capped_at_8) {
    CHECK_EQ(withEnv(nullptr), std::min(8u, hw()));
    CHECK_EQ(withEnv(""), std::min(8u, hw()));
}

TEST(parallel_thread_count_env_override) {
    CHECK_EQ(withEnv("1"), 1u);
    CHECK_EQ(withEnv("2"), std::min(2u, hw()));
    CHECK_EQ(withEnv("100000"), hw()); // clamped to hardware threads, not to 8
}

TEST(parallel_thread_count_invalid_env_falls_back_to_default) {
    for (const char *bad : {"abc", "0", "-3", "  "}) CHECK_EQ(withEnv(bad), std::min(8u, hw()));
    unsetenv(ENV);
}

TEST(parallel_for_visits_each_index_exactly_once) {
    setenv(ENV, "4", 1);
    constexpr size_t N = 1000;
    std::vector<std::atomic<int>> hits(N);
    parallelFor(N, [&](size_t i) { hits[i]++; }, ENV);
    for (size_t i = 0; i < N; i++) CHECK_EQ(hits[i].load(), 1);
    unsetenv(ENV);
}

TEST(parallel_for_zero_count_never_calls) {
    int calls = 0;
    parallelFor(0, [&](size_t) { calls++; }, ENV);
    CHECK_EQ(calls, 0);
}

TEST(parallel_for_single_thread_runs_in_order_on_caller) {
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
    unsetenv(ENV);
}

TEST(parallel_for_propagates_exception_and_still_finishes_other_work) {
    setenv(ENV, "4", 1);
    constexpr size_t N = 200;
    std::atomic<int> done{0};
    CHECK_THROWS(parallelFor(N, [&](size_t i) {
        done++;
        if (i == 50) throw std::runtime_error("boom");
    }, ENV), std::runtime_error);
    CHECK_EQ(done.load(), (int)N); // workers keep draining after a failure
    unsetenv(ENV);
}
