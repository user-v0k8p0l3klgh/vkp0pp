#include <cstdio>
#include <cstring>

#include "test.h"

int main(int argc, char** argv) {
    const char* filter = argc > 1 ? argv[1] : nullptr;
    int run = 0;
    for (const auto& c : bll::test::registry()) {
        if (filter && !std::strstr(c.name, filter)) continue;
        const int before = bll::test::failures();
        c.fn();
        ++run;
        std::printf("%s %s\n", bll::test::failures() == before ? "ok  " : "FAIL", c.name);
    }
    std::printf("%d tests, %d failed checks\n", run, bll::test::failures());
    return bll::test::failures() == 0 && run > 0 ? 0 : 1;
}
