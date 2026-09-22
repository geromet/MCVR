#include "harness.hpp"
#include "thread_env_protocol.hpp"

#include "core/util/parallel.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <sys/stat.h>
#include <unistd.h>

namespace {
std::string runningExecutableIdentity() {
    struct stat st {};
    if (stat("/proc/self/exe", &st) != 0) return {};
    return std::to_string(static_cast<unsigned long long>(st.st_dev)) + ":" +
           std::to_string(static_cast<unsigned long long>(st.st_ino)) + ":" +
           std::to_string(static_cast<unsigned long long>(st.st_size));
}

int runThreadEnvChild() {
    constexpr const char *ENV = "MCVR_TEST_THREADS";
    const char *attempt = std::getenv("MCVR_TEST_ATTEMPT");
    const char *caseId = std::getenv("MCVR_TEST_CASE");
    const char *expectedExe = std::getenv("MCVR_TEST_EXEC_ID");
    const char *gateText = std::getenv("MCVR_TEST_GATE_FD");
    const char *value = std::getenv(ENV);
    if (!attempt || !caseId || !expectedExe || !gateText) return 90;

    char *gateEnd = nullptr;
    const long gate = std::strtol(gateText, &gateEnd, 10);
    if (!gateEnd || *gateEnd != '\0' || gate < 0) return 91;
    const std::string actualExe = runningExecutableIdentity();
    if (actualExe.empty() || actualExe != expectedExe) return 92;

    const bool present = value != nullptr;
    const uint32_t result = mcvr::parallelThreadCount(ENV);
    const thread_env_proof::Record record{
        attempt, caseId, present, present ? std::string(value) : std::string{}, result, actualExe};
    const std::string encoded = thread_env_proof::serialize(record);
    if (std::fwrite(encoded.data(), 1, encoded.size(), stdout) != encoded.size()) return 93;
    std::fflush(stdout);

    char release = 0;
    if (read(static_cast<int>(gate), &release, 1) != 1 || release != 'R') return 94;
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
