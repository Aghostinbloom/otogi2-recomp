#include "host/stacks.hpp"

#include <windows.h>
#include <dbghelp.h>
#include <tlhelp32.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <thread>

namespace otogi {
namespace {

// Lifted functions keep large frames, so return addresses can be far apart.
constexpr int kMaxFrames = 32;
constexpr int kScanQwords = 65536;

struct Sample {
    DWORD tid;
    uint64_t pc;
    uint64_t returns[kMaxFrames];
    int count;
};

uintptr_t g_image_lo, g_image_hi;

void find_image() {
    auto *base = reinterpret_cast<const uint8_t *>(GetModuleHandleW(nullptr));
    auto *dos = reinterpret_cast<const IMAGE_DOS_HEADER *>(base);
    auto *nt = reinterpret_cast<const IMAGE_NT_HEADERS *>(base + dos->e_lfanew);
    g_image_lo = reinterpret_cast<uintptr_t>(base);
    g_image_hi = g_image_lo + nt->OptionalHeader.SizeOfImage;
}

// While the thread is suspended nothing here may take a lock or allocate: it
// could be holding the one we would wait on. So only copy: the PC, and the
// values on its stack that point into the exe (a scan, so it over-reports).
bool capture(DWORD tid, Sample &out) {
    HANDLE thread = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT, FALSE, tid);
    if (!thread)
        return false;
    bool ok = false;
    if (SuspendThread(thread) != static_cast<DWORD>(-1)) {
        CONTEXT ctx{};
        ctx.ContextFlags = CONTEXT_CONTROL;
        if (GetThreadContext(thread, &ctx)) {
            out.tid = tid;
            out.pc = ctx.Rip;
            out.count = 0;
            MEMORY_BASIC_INFORMATION mbi{};
            if (VirtualQuery(reinterpret_cast<void *>(ctx.Rsp), &mbi, sizeof mbi)) {
                auto *sp = reinterpret_cast<const uint64_t *>(ctx.Rsp);
                auto *end = reinterpret_cast<const uint64_t *>(static_cast<const uint8_t *>(mbi.BaseAddress) +
                                                               mbi.RegionSize);
                for (int i = 0; i < kScanQwords && sp + i < end && out.count < kMaxFrames; i++) {
                    if (sp[i] > g_image_lo && sp[i] < g_image_hi)
                        out.returns[out.count++] = sp[i];
                }
            }
            ok = true;
        }
        ResumeThread(thread);
    }
    CloseHandle(thread);
    return ok;
}

void print_symbol(uint64_t pc) {
    alignas(SYMBOL_INFO) char buf[sizeof(SYMBOL_INFO) + 256] = {};
    auto *sym = reinterpret_cast<SYMBOL_INFO *>(buf);
    sym->SizeOfStruct = sizeof(SYMBOL_INFO);
    sym->MaxNameLen = 255;
    DWORD64 disp = 0;
    if (SymFromAddr(GetCurrentProcess(), pc, &disp, sym))
        std::fprintf(stderr, "    %s+0x%llX\n", sym->Name, static_cast<unsigned long long>(disp));
    else
        std::fprintf(stderr, "    0x%llX\n", static_cast<unsigned long long>(pc));
}

void dump_all_threads() {
    static Sample samples[256];
    int n = 0;
    const DWORD self = GetCurrentThreadId(), pid = GetCurrentProcessId();
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snap == INVALID_HANDLE_VALUE)
        return;
    THREADENTRY32 te{};
    te.dwSize = sizeof te;
    for (BOOL more = Thread32First(snap, &te); more && n < 256; more = Thread32Next(snap, &te)) {
        if (te.th32OwnerProcessID == pid && te.th32ThreadID != self && capture(te.th32ThreadID, samples[n]))
            n++;
    }
    CloseHandle(snap);

    std::fprintf(stderr, "[STACKS] %d threads; those in the exe (pc, then exe addresses on the stack):\n", n);
    for (int i = 0; i < n; i++) {
        const bool pc_in_exe = samples[i].pc > g_image_lo && samples[i].pc < g_image_hi;
        if (!pc_in_exe && !samples[i].count)
            continue;
        std::fprintf(stderr, "  thread %lu:\n", samples[i].tid);
        print_symbol(samples[i].pc);
        for (int k = 0; k < samples[i].count; k++)
            print_symbol(samples[i].returns[k]);
    }
    std::fflush(stderr);
}

void find_image_once() {
    static std::once_flag once;
    std::call_once(once, find_image);
}

}  // namespace

void dump_stacks_now() {
    find_image_once();
    dump_all_threads();
}

void start_stack_sampler() {
    const char *every = std::getenv("OTOGI_STACKS_EVERY");
    const int seconds = every ? std::atoi(every) : 0;
    if (seconds <= 0)
        return;
    find_image_once();
    std::thread([seconds] {
        for (;;) {
            Sleep(static_cast<DWORD>(seconds) * 1000);
            dump_all_threads();
        }
    }).detach();
}

}  // namespace otogi
