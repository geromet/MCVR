#include "harness.hpp"

#include <cstdlib>
#include <fcntl.h>
#include <string>
#include <sys/stat.h>
#include <unistd.h>

namespace {

std::string fdIdentity(int fd) {
    struct stat st {};
    if (fstat(fd, &st) != 0) return {};
    return std::to_string(static_cast<unsigned long long>(st.st_dev)) + ":" +
           std::to_string(static_cast<unsigned long long>(st.st_ino)) + ":" +
           std::to_string(static_cast<unsigned long long>(st.st_size));
}

std::string pathIdentity(const char *path) {
    struct stat st {};
    if (stat(path, &st) != 0) return {};
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
    return fsync(destination) == 0;
}

bool pathStillNamesAuthorizedObject(const char *path, const std::string &authorizedIdentity) {
    const std::string current = pathIdentity(path);
    return !current.empty() && current == authorizedIdentity;
}

} // namespace

TEST(thread_env_proof_same_path_post_measurement_replacement_is_rejected) {
#ifdef __linux__
    char authorizedPath[] = "/tmp/mcvr-proof-authorized-XXXXXX";
    char replacementPath[] = "/tmp/mcvr-proof-replacement-XXXXXX";
    const int authorized = mkstemp(authorizedPath);
    const int replacement = mkstemp(replacementPath);
    CHECK(authorized >= 0);
    CHECK(replacement >= 0);
    if (authorized < 0 || replacement < 0) {
        if (authorized >= 0) close(authorized);
        if (replacement >= 0) close(replacement);
        return;
    }

    CHECK(copySelfTo(authorized));
    CHECK(copySelfTo(replacement));
    const std::string measured = fdIdentity(authorized);
    CHECK(!measured.empty());
    CHECK_EQ(pathIdentity(authorizedPath), measured);

    // The apparent command/path is unchanged, but rename atomically substitutes a
    // different executable object after measurement. Acceptance must therefore
    // fail on object identity even though the replacement bytes are identical.
    CHECK(rename(replacementPath, authorizedPath) == 0);
    CHECK(!pathStillNamesAuthorizedObject(authorizedPath, measured));
    CHECK_EQ(fdIdentity(authorized), measured);

    close(replacement);
    close(authorized);
    unlink(authorizedPath);
    unlink(replacementPath);
#else
    CHECK(true);
#endif
}
