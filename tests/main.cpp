#include "harness.hpp"

#include "core/util/parallel.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

namespace {
int runThreadEnvChild() {
    constexpr const char *ENV = "MCVR_TEST_THREADS";
    const char *attempt = std::getenv("MCVR_TEST_ATTEMPT");
    const char *caseId = std::getenv("MCVR_TEST_CASE");
    const char *value = std::getenv(ENV);
    if (!attempt || !caseId) return 90;

    const bool present = value != nullptr;
    const std::string observed = present ? value : "<unset>";
    const uint32_t result = mcvr::parallelThreadCount(ENV);
    std::printf("attempt=%s\ncase=%s\npresent=%d\nvalue=%s\nresult=%u\n",
                attempt, caseId, present ? 1 : 0, observed.c_str(), result);
    return 0;
}
} // namespace

int main(int argc, char **argv) {
    if (argc == 2 && std::strcmp(argv[1], "--thread-env-child") == 0) return runThreadEnvChild();

    const char *filter = argc > 1 ? argv[1] : nullptr;
    int ran = 0, failed = 0;
    for (auto &c : mt::registry()) {
        if (filter && !std::strstr(c.name, filter)) continue;
        int before = mt::failures();
        try {
            c.fn();
        } catch (const std::exception &e) {
            std::fprintf(stderr, "    unexpected exception: %s\n", e.what());
            ++mt::failures();
        } catch (...) {
            std::fprintf(stderr, "    unexpected non-std exception\n");
            ++mt::failures();
        }
        bool ok = mt::failures() == before;
        std::printf("[%s] %s\n", ok ? " OK " : "FAIL", c.name);
        ++ran;
        if (!ok) ++failed;
    }
    std::printf("%d run, %d failed\n", ran, failed);
    return failed == 0 && ran > 0 ? 0 : 1;
}
