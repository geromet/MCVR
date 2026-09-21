#include "harness.hpp"

#include <algorithm>
#include <cstdlib>
#include <string>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
#include <vector>

extern char **environ;

namespace {
constexpr const char *KEY = "MCVR_TEST_THREADS";
struct ChildResult { int status = -1; std::string output; };

bool selectedKeyEntry(const std::string &entry) {
    const std::string prefix = std::string(KEY) + "=";
    return entry.rfind(prefix, 0) == 0 || entry.rfind("MCVR_TEST_ATTEMPT=", 0) == 0 || entry.rfind("MCVR_TEST_CASE=", 0) == 0;
}

std::string selfExe() {
    std::vector<char> buf(4096);
    const ssize_t n = readlink("/proc/self/exe", buf.data(), buf.size() - 1);
    return n > 0 ? std::string(buf.data(), static_cast<size_t>(n)) : std::string{};
}

std::vector<std::string> explicitEnvironment(const char *fixture, const std::string &attempt, const std::string &caseId) {
    std::vector<std::string> env;
    for (char **p = environ; p && *p; ++p) {
        std::string entry(*p);
        if (!selectedKeyEntry(entry)) env.push_back(std::move(entry));
    }
    if (fixture) env.emplace_back(std::string(KEY) + "=" + fixture);
    env.emplace_back("MCVR_TEST_ATTEMPT=" + attempt);
    env.emplace_back("MCVR_TEST_CASE=" + caseId);
    return env;
}

ChildResult runChild(const std::vector<std::string> &ownedEnv) {
    const std::string exe = selfExe();
    if (exe.empty()) return {};
    int fds[2];
    if (pipe(fds) != 0) return {};
    std::vector<char *> envp;
    for (const auto &entry : ownedEnv) envp.push_back(const_cast<char *>(entry.c_str()));
    envp.push_back(nullptr);
    const pid_t pid = fork();
    if (pid == 0) {
        close(fds[0]); dup2(fds[1], STDOUT_FILENO); close(fds[1]);
        char *const argv[] = {const_cast<char *>(exe.c_str()), const_cast<char *>("--thread-env-child"), nullptr};
        execve(exe.c_str(), argv, envp.data());
        _exit(127);
    }
    close(fds[1]);
    ChildResult result;
    char buf[512];
    ssize_t n;
    while ((n = read(fds[0], buf, sizeof(buf))) > 0) result.output.append(buf, static_cast<size_t>(n));
    close(fds[0]);
    int status = 0;
    if (pid > 0 && waitpid(pid, &status, 0) == pid && WIFEXITED(status)) result.status = WEXITSTATUS(status);
    return result;
}

void checkBound(const ChildResult &r, const std::string &attempt, const std::string &caseId, bool present, const std::string &value, uint32_t expected) {
    CHECK_EQ(r.status, 0);
    CHECK(r.output.find("attempt=" + attempt + "\n") != std::string::npos);
    CHECK(r.output.find("case=" + caseId + "\n") != std::string::npos);
    CHECK(r.output.find(std::string("present=") + (present ? "1\n" : "0\n")) != std::string::npos);
    CHECK(r.output.find("value=" + value + "\n") != std::string::npos);
    CHECK(r.output.find("result=" + std::to_string(expected) + "\n") != std::string::npos);
}
uint32_t hw() { return std::max(1u, std::thread::hardware_concurrency()); }
uint32_t defaultThreads() { return std::min(8u, hw()); }
} // namespace

TEST(thread_env_child_explicit_fixture_ignores_conflicting_parent) {
    setenv(KEY, "99", 1);
    auto one = explicitEnvironment("1", "attempt-a", "case-one");
    setenv(KEY, "77", 1);
    checkBound(runChild(one), "attempt-a", "case-one", true, "1", 1u);
    setenv(KEY, "2", 1);
    auto empty = explicitEnvironment("", "attempt-b", "case-empty");
    unsetenv(KEY);
    checkBound(runChild(empty), "attempt-b", "case-empty", true, "", defaultThreads());
    setenv(KEY, "2", 1);
    auto missing = explicitEnvironment(nullptr, "attempt-c", "case-unset");
    setenv(KEY, "1", 1);
    checkBound(runChild(missing), "attempt-c", "case-unset", false, "<unset>", defaultThreads());
}

TEST(thread_env_inherited_snapshot_control_is_mechanically_sensitive) {
    setenv(KEY, "1", 1);
    auto snapshotA = explicitEnvironment(std::getenv(KEY), "snapshot-a", "control-a");
    setenv(KEY, "2", 1);
    auto snapshotB = explicitEnvironment(std::getenv(KEY), "snapshot-b", "control-b");
    checkBound(runChild(snapshotA), "snapshot-a", "control-a", true, "1", 1u);
    checkBound(runChild(snapshotB), "snapshot-b", "control-b", true, "2", std::min(2u, hw()));
}
