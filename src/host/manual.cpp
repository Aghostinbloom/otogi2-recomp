// Hooks the lifted code calls back into: hand-written overrides, and reports for
// indirect calls and instructions the lifter could not translate.

#include "host/runtime.hpp"

#include <cstdio>
#include <cstdlib>

extern "C" recomp_func_t recomp_lookup_manual(uint32_t xbox_va) {
    (void)xbox_va;
    return nullptr;
}

extern "C" void recomp_icall_fail_log(uint32_t va) {
    std::fprintf(stderr, "[ICALL] cannot resolve 0x%08X (call #%llu); recent targets:",
                 va, static_cast<unsigned long long>(g_icall_count));
    for (uint32_t i = 0; i < 16; i++) {
        const uint32_t t = g_icall_trace[(g_icall_trace_idx - 16 + i) & 15];
        if (t)
            std::fprintf(stderr, " %08X", t);
    }
    std::fprintf(stderr, "\n");
    std::fflush(stderr);
}

// A wild pointer reached a call site. Reported at hit 1, 10, 100... per address:
// the progression is what tells a one-off from a loop.
extern "C" void recomp_icall_not_code_log(uint32_t va) {
    constexpr int kSlots = 16;
    static uint32_t seen[kSlots];
    static uint64_t hits[kSlots];
    static int count;

    int i = 0;
    while (i < count && seen[i] != va)
        i++;
    if (i == count) {
        if (count == kSlots)
            return;
        seen[count] = va;
        hits[count++] = 0;
    }
    uint64_t n = ++hits[i];
    while (n >= 10 && n % 10 == 0)
        n /= 10;
    if (n != 1)
        return;
    std::fprintf(stderr, "[ICALL] target 0x%08X is not code, skipped %llu time(s)\n",
                 va, static_cast<unsigned long long>(hits[i]));
    std::fflush(stderr);
}

extern "C" void recomp_unimpl(const char *text, uint32_t va) {
    static int printed;
    const char *trap = std::getenv("RECOMP_UNIMPL_TRAP");
    const bool stop = trap && *trap && *trap != '0';
    if (printed < 50 || stop) {
        printed++;
        std::fprintf(stderr, "[UNIMPL] untranslated `%s` reached at 0x%08X (no-op)\n", text, va);
        std::fflush(stderr);
    }
    if (stop)
        std::abort();
}
