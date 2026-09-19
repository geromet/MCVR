#include "harness.hpp"

#include <cstring>

int main(int argc, char **argv) {
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
