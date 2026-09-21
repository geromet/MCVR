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

enum class ExecutableFixture { Exact, SamePathReplacement };

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

bool copySelfTo(int destination) {
    const int source = open("/proc/self/exe", O_RDONLY | O_CLOEXEC);
    if (source < 0) return false;
    char buffer[16384];
    for (;;) {
        const ssize_t n = read(source, buffer, sizeof(buffer));
        if (n == 0) break;
        if (n < 0) { close(source); return false; }
        ssize_t written = 0;
        while (written < n) {
            const ssize_t w = write(destination, buffer + written, static_cast<size_t>(n - written));
            if (w <= 0) { close(source); return false; }
            written += w;
        }
    }
    close(source);
    return lseek(destination, 0, SEEK_SET) >= 0;
}

int immutableSelfExecutable() {
    const int image = memfd_create("mcvr-thread-env-proof", MFD_ALLOW_SEALING | MFD_CLOEXEC);
    if (image < 0) return -1;
    if (!copySelfTo(image) || fchmod(image, 0500) != 0 ||
        fcntl(image, F_ADD_SEALS, F_SEAL_WRITE | F_SEAL_GROW | F_SEAL_SHRINK | F_SEAL_SEAL) != 0) {
        close(image);
        return -1;
    }
    return image;
}

// Create two byte-identical executable objects, authorize the first object, then
// atomically replace its path with the second object before opening the launch
// fd. This makes the authorization -> replacement -> spawn order mechanical.
bool samePathReplacement(int &authorized, int &launch) {
    char authorizedPath[] = "/tmp/mcvr-proof-authorized-XXXXXX";
    char replacementPath[] = "/tmp/mcvr-proof-replacement-XXXXXX";
    authorized = mkstemp(authorizedPath);
    int replacement = mkstemp(replacementPath);
    if (authorized < 0 || replacement < 0) {
        if (authorized >= 0) close(authorized);
        if (replacement >= 0) close(replacement);
        authorized = -1;
        return false;
    }
    bool ok = copySelfTo(authorized) && copySelfTo(replacement) &&
              fchmod(authorized, 0500) == 0 && fchmod(replacement, 0500) == 0;
    const std::string measured = ok ? fdIdentity(authorized) : std::string{};
    if (ok) ok = !measured.empty() && rename(replacementPath, authorizedPath) == 0;
    close(replacement);
    if (ok) launch = open(authorizedPath, O_RDONLY | O_CLOEXEC);
    unlink(authorizedPath);
    unlink(replacementPath);
    if (!ok || launch < 0 || fdIdentity(launch) == measured) {
        if (launch >= 0) close(launch);
        close(authorized);
        authorized = launch = -1;
        return false;
    }
    return true;
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
                     const std::optional<std::string> &expectedIdentityOverride = std::nullopt,
                     ExecutableFixture executableFixture = ExecutableFixture::Exact) {
    ChildResult result;
    int authorized = -1;
    int launch = -1;
    if (executableFixture == ExecutableFixture::SamePathReplacement) {
        if (!samePathReplacement(authorized, launch)) return result;
    } else {
        authorized = immutableSelfExecutable();
        launch = authorized;
        if (authorized < 0) return result;
    }
    result.authorizedExecutable = fdIdentity(authorized);
    const std::string launchIdentity = fdIdentity(launch);
    const std::string expectedIdentity = expectedIdentityOverride.value_or(launchIdentity);

    int outputPipe[2];
    int gatePipe[2];
    if (pipe(outputPipe) != 0 || pipe(gatePipe) != 0) {
        if (launch != authorized) close(launch);
        close(authorized);
        return result;
    }
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
        fexecve(launch, argv, envp.data());
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
    if (launch != authorized) close(launch);
    close(authorized);
    return result;
}

bool acceptsBound(const ChildResult &r, const std::string &attempt, const std::string &caseId,
                  bool present, const std::string &value, uint32_t expected,
                  bool enforceExecutableAssociation = true) {
    if (r.status != 0) return false;
    const auto record = thread_env_proof::parse(r.output);
    if (!record || record->attempt != attempt || record->caseId != caseId ||
        record->present != present || record->value != value || record->result != expected)
        return false;
    if (!enforceExecutableAssociation) return true;
    return r.parentBinding && record->executableIdentity == r.authorizedExecutable &&
           record->executableIdentity == r.parentObservedExecutable;
}

void checkBound(const ChildResult &r, const std::string &attempt, const std::string &caseId,
                bool present, const std::string &value, uint32_t expected) {
    CHECK(acceptsBound(r, attempt, caseId, present, value, expected));
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
    CHECK(!acceptsBound(stale, "attempt-stale", "case-stale", true, "1", 1u));

    const auto wrong = runChild("1", "attempt-wrong", "case-wrong", std::string("0:0:0"));
    CHECK(!acceptsBound(wrong, "attempt-wrong", "case-wrong", true, "1", 1u));
}

TEST(thread_env_proof_same_path_replacement_uses_canonical_acceptance_oracle) {
#ifdef __linux__
    const auto exact = runChild("2", "attempt-exact", "case-exact");
    CHECK(acceptsBound(exact, "attempt-exact", "case-exact", true, "2", std::min(2u, hw())));

    const auto replaced = runChild("2", "attempt-replaced", "case-replaced", std::nullopt,
                                   ExecutableFixture::SamePathReplacement);
    CHECK_EQ(replaced.status, 0);
    CHECK(!replaced.parentObservedExecutable.empty());
    CHECK(replaced.parentObservedExecutable != replaced.authorizedExecutable);
    CHECK(!acceptsBound(replaced, "attempt-replaced", "case-replaced", true, "2", std::min(2u, hw())));

    // Causality/sensitivity: all non-association evidence is accepted when only
    // the executable-object association validator is deliberately bypassed.
    CHECK(acceptsBound(replaced, "attempt-replaced", "case-replaced", true, "2", std::min(2u, hw()), false));
#else
    CHECK(true);
#endif
}
