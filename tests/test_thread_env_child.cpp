#include "harness.hpp"
#include "thread_env_protocol.hpp"

#include <algorithm>
#include <cstdlib>
#include <fcntl.h>
#include <linux/memfd.h>
#include <optional>
#include <string>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
#include <vector>

namespace {
constexpr const char *KEY = "MCVR_TEST_THREADS";

struct ChildResult {
    int status = -1;
    std::string output;
    std::string authorizedExecutable;
    std::string parentObservedExecutable;
    bool parentBinding = false;
};

std::string fdIdentity(int fd) {
    struct stat st {};
    if (fstat(fd, &st) != 0) return {};
    return std::to_string(static_cast<unsigned long long>(st.st_dev)) + ":" +
           std::to_string(static_cast<unsigned long long>(st.st_ino)) + ":" +
           std::to_string(static_cast<unsigned long long>(st.st_size));
}

std::string processExecutableIdentity(pid_t pid) {
    const std::string path = "/proc/" + std::to_string(pid) + "/exe";
    struct stat st {};
    if (stat(path.c_str(), &st) != 0) return {};
    return std::to_string(static_cast<unsigned long long>(st.st_dev)) + ":" +
           std::to_string(static_cast<unsigned long long>(st.st_ino)) + ":" +
           std::to_string(static_cast<unsigned long long>(st.st_size));
}

int immutableSelfExecutable() {
    const int source = open("/proc/self/exe", O_RDONLY | O_CLOEXEC);
    if (source < 0) return -1;
    const int image = memfd_create("mcvr-thread-env-proof", MFD_ALLOW_SEALING | MFD_CLOEXEC);
    if (image < 0) { close(source); return -1; }
    char buffer[16384];
    for (;;) {
        const ssize_t n = read(source, buffer, sizeof(buffer));
        if (n == 0) break;
        if (n < 0) { close(source); close(image); return -1; }
        ssize_t written = 0;
        while (written < n) {
            const ssize_t w = write(image, buffer + written, static_cast<size_t>(n - written));
            if (w <= 0) { close(source); close(image); return -1; }
            written += w;
        }
    }
    close(source);
    if (fchmod(image, 0500) != 0 || lseek(image, 0, SEEK_SET) < 0 ||
        fcntl(image, F_ADD_SEALS, F_SEAL_WRITE | F_SEAL_GROW | F_SEAL_SHRINK | F_SEAL_SEAL) != 0) {
        close(image);
        return -1;
    }
    return image;
}

std::vector<std::string> explicitEnvironment(const char *fixture, const std::string &attempt,
                                              const std::string &caseId, const std::string &exeIdentity,
                                              int gateFd) {
    std::vector<std::string> env{"LANG=C", "LC_ALL=C"};
    if (fixture) env.emplace_back(std::string(KEY) + "=" + fixture);
    env.emplace_back("MCVR_TEST_ATTEMPT=" + attempt);
    env.emplace_back("MCVR_TEST_CASE=" + caseId);
    env.emplace_back("MCVR_TEST_EXEC_ID=" + exeIdentity);
    env.emplace_back("MCVR_TEST_GATE_FD=" + std::to_string(gateFd));
    return env;
}

ChildResult runChild(const char *fixture, const std::string &attempt, const std::string &caseId,
                     const std::optional<std::string> &expectedIdentityOverride = std::nullopt) {
    ChildResult result;
    const int image = immutableSelfExecutable();
    if (image < 0) return result;
    result.authorizedExecutable = fdIdentity(image);
    const std::string expectedIdentity = expectedIdentityOverride.value_or(result.authorizedExecutable);

    int outputPipe[2];
    int gatePipe[2];
    if (pipe(outputPipe) != 0 || pipe(gatePipe) != 0) { close(image); return result; }
    auto ownedEnv = explicitEnvironment(fixture, attempt, caseId, expectedIdentity, gatePipe[0]);
    std::vector<char *> envp;
    for (auto &entry : ownedEnv) envp.push_back(entry.data());
    envp.push_back(nullptr);

    const pid_t pid = fork();
    if (pid == 0) {
        close(outputPipe[0]);
        close(gatePipe[1]);
        dup2(outputPipe[1], STDOUT_FILENO);
        close(outputPipe[1]);
        char *const argv[] = {const_cast<char *>("mcvr-thread-env-proof"),
                              const_cast<char *>("--thread-env-child"), nullptr};
        fexecve(image, argv, envp.data());
        _exit(127);
    }
    close(outputPipe[1]);
    close(gatePipe[0]);

    char buffer[512];
    while (pid > 0 && result.output.find("end=1\n") == std::string::npos) {
        const ssize_t n = read(outputPipe[0], buffer, sizeof(buffer));
        if (n <= 0) break;
        result.output.append(buffer, static_cast<size_t>(n));
    }
    if (pid > 0 && result.output.find("end=1\n") != std::string::npos) {
        result.parentObservedExecutable = processExecutableIdentity(pid);
        result.parentBinding = !result.parentObservedExecutable.empty() &&
                               result.parentObservedExecutable == result.authorizedExecutable;
        const char release = 'R';
        (void)write(gatePipe[1], &release, 1);
    }
    close(gatePipe[1]);
    while (true) {
        const ssize_t n = read(outputPipe[0], buffer, sizeof(buffer));
        if (n <= 0) break;
        result.output.append(buffer, static_cast<size_t>(n));
    }
    close(outputPipe[0]);
    int status = 0;
    if (pid > 0 && waitpid(pid, &status, 0) == pid && WIFEXITED(status)) result.status = WEXITSTATUS(status);
    close(image);
    return result;
}

void checkBound(const ChildResult &r, const std::string &attempt, const std::string &caseId,
                bool present, const std::string &value, uint32_t expected) {
    CHECK_EQ(r.status, 0);
    CHECK(r.parentBinding);
    const auto record = thread_env_proof::parse(r.output);
    CHECK(record.has_value());
    if (!record) return;
    CHECK_EQ(record->attempt, attempt);
    CHECK_EQ(record->caseId, caseId);
    CHECK_EQ(record->present, present);
    CHECK_EQ(record->value, value);
    CHECK_EQ(record->result, expected);
    CHECK_EQ(record->executableIdentity, r.authorizedExecutable);
    CHECK_EQ(record->executableIdentity, r.parentObservedExecutable);
}

uint32_t hw() { return std::max(1u, std::thread::hardware_concurrency()); }
uint32_t defaultThreads() { return std::min(8u, hw()); }
} // namespace

TEST(thread_env_child_explicit_fixture_ignores_conflicting_parent) {
    setenv(KEY, "99", 1);
    const auto one = runChild("1", "attempt-a", "case-one");
    setenv(KEY, "77", 1);
    checkBound(one, "attempt-a", "case-one", true, "1", 1u);

    setenv(KEY, "2", 1);
    const auto empty = runChild("", "attempt-b", "case-empty");
    unsetenv(KEY);
    checkBound(empty, "attempt-b", "case-empty", true, "", defaultThreads());

    setenv(KEY, "2", 1);
    const auto missing = runChild(nullptr, "attempt-c", "case-unset");
    setenv(KEY, "1", 1);
    checkBound(missing, "attempt-c", "case-unset", false, "", defaultThreads());
}

TEST(thread_env_child_closed_environment_rejects_parent_poison) {
    setenv("MCVR_UNDECLARED_POISON", "must-not-cross", 1);
    const auto r = runChild("2", "attempt-poison", "case-poison");
    checkBound(r, "attempt-poison", "case-poison", true, "2", std::min(2u, hw()));
    CHECK(r.output.find("must-not-cross") == std::string::npos);
    unsetenv("MCVR_UNDECLARED_POISON");
}

TEST(thread_env_inherited_snapshot_control_is_mechanically_sensitive) {
    setenv(KEY, "1", 1);
    const std::string snapshotA = std::getenv(KEY);
    setenv(KEY, "2", 1);
    const std::string snapshotB = std::getenv(KEY);
    checkBound(runChild(snapshotA.c_str(), "snapshot-a", "control-a"), "snapshot-a", "control-a", true, "1", 1u);
    checkBound(runChild(snapshotB.c_str(), "snapshot-b", "control-b"), "snapshot-b", "control-b", true, "2", std::min(2u, hw()));
}

TEST(thread_env_proof_codec_is_injective_and_fail_closed) {
    const thread_env_proof::Record record{"a\n=b", "case\t\\", true, "2\nresult=999\r", 7u, "dev:ino:size"};
    const std::string encoded = thread_env_proof::serialize(record);
    const auto parsed = thread_env_proof::parse(encoded);
    CHECK(parsed.has_value());
    if (parsed) {
        CHECK_EQ(parsed->attempt, record.attempt);
        CHECK_EQ(parsed->caseId, record.caseId);
        CHECK_EQ(parsed->value, record.value);
    }
    CHECK(!thread_env_proof::parse(encoded + "extra=1\n").has_value());
    std::string uppercase = encoded;
    const auto valuePos = uppercase.find("value=");
    if (valuePos != std::string::npos) {
        for (size_t i = valuePos + 6; i < uppercase.size() && uppercase[i] != '\n'; ++i)
            if (uppercase[i] >= 'a' && uppercase[i] <= 'f') { uppercase[i] = static_cast<char>(uppercase[i] - 'a' + 'A'); break; }
    }
    CHECK(!thread_env_proof::parse(uppercase).has_value());
    CHECK(!thread_env_proof::parse(encoded.substr(0, encoded.size() - 1)).has_value());
}

TEST(thread_env_proof_rejects_wrong_or_stale_executable_identity) {
    const auto first = runChild("1", "attempt-first", "case-first");
    checkBound(first, "attempt-first", "case-first", true, "1", 1u);

    const auto stale = runChild("1", "attempt-stale", "case-stale", first.authorizedExecutable);
    CHECK(stale.status != 0);
    CHECK(!stale.parentBinding || stale.output.empty());

    const auto wrong = runChild("1", "attempt-wrong", "case-wrong", std::string("0:0:0"));
    CHECK(wrong.status != 0);
    CHECK(wrong.output.empty());
}
