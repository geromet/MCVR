#include "harness.hpp"

#include "common/singleton.hpp"

#include <atomic>
#include <stdexcept>
#include <thread>
#include <vector>

// Each test uses its own Derived type: Singleton state is per-type static.

namespace {
struct A : Singleton<A> {
    explicit A(int v) : value(v) {}
    int value;
    friend class Singleton<A>;
};
struct B : Singleton<B> {
    B() = default;
    friend class Singleton<B>;
};
struct ThrowOnce : Singleton<ThrowOnce> {
    explicit ThrowOnce(bool fail) {
        if (fail) throw std::runtime_error("ctor failed");
    }
    friend class Singleton<ThrowOnce>;
};
struct Racy : Singleton<Racy> {
    Racy() { ++constructed; }
    static inline std::atomic<int> constructed{0};
    friend class Singleton<Racy>;
};
} // namespace

TEST(singleton_uninitialized_state) {
    CHECK(!B::is_initialized());
    CHECK(B::try_instance() == nullptr);
    CHECK_THROWS(B::instance(), std::logic_error);
}

TEST(singleton_init_forwards_args_and_returns_same_instance) {
    A::init(42);
    CHECK(A::is_initialized());
    CHECK_EQ(A::instance().value, 42);
    CHECK(&A::instance() == A::try_instance());
}

TEST(singleton_double_init_throws_and_keeps_first) {
    CHECK_THROWS(A::init(7), std::logic_error);
    CHECK_EQ(A::instance().value, 42);
}

TEST(singleton_failed_construction_leaves_uninitialized_and_allows_retry) {
    CHECK_THROWS(ThrowOnce::init(true), std::runtime_error);
    CHECK(!ThrowOnce::is_initialized());
    ThrowOnce::init(false);
    CHECK(ThrowOnce::is_initialized());
}

TEST(singleton_concurrent_init_constructs_exactly_once) {
    constexpr int N = 8;
    std::atomic<bool> go{false};
    std::atomic<int> ok{0}, rejected{0};
    std::vector<std::thread> ts;
    for (int i = 0; i < N; i++) {
        ts.emplace_back([&] {
            while (!go.load()) std::this_thread::yield();
            try {
                Racy::init();
                ++ok;
            } catch (const std::logic_error &) { ++rejected; }
        });
    }
    go = true;
    for (auto &t : ts) t.join();
    CHECK_EQ(Racy::constructed.load(), 1);
    CHECK_EQ(ok.load(), 1);
    CHECK_EQ(rejected.load(), N - 1);
}
