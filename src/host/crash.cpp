#include "host/crash.hpp"

#include "host/runtime.hpp"

#include <dbghelp.h>

#include <cstdio>

namespace otogi {
namespace {

void print_guest_function(void *rip) {
    alignas(SYMBOL_INFO) char buf[sizeof(SYMBOL_INFO) + 256] = {};
    auto *sym = reinterpret_cast<SYMBOL_INFO *>(buf);
    sym->SizeOfStruct = sizeof(SYMBOL_INFO);
    sym->MaxNameLen = 255;
    DWORD64 disp = 0;
    if (SymFromAddr(GetCurrentProcess(), reinterpret_cast<DWORD64>(rip), &disp, sym))
        std::fprintf(stderr, "  in %s+0x%llX\n", sym->Name, static_cast<unsigned long long>(disp));
}

// Lifted calls push the guest return address, so the guest stack still holds
// the call chain. It is a scan, not a frame walk: stale return addresses below
// esp show up too.
void print_guest_stack() {
    if (!g_xbox_mem_offset || !g_esp)
        return;
    const auto *sp = reinterpret_cast<const uint32_t *>(g_xbox_mem_offset + g_esp);
    int shown = 0;
    std::fprintf(stderr, "  guest stack (return addresses, innermost first):\n");
    for (int i = 0; i < 256 && shown < 24; i++) {
        if (sp[i] > g_xbox_code_lo && sp[i] < g_xbox_code_hi) {
            std::fprintf(stderr, "    [esp+%-4d] 0x%08X\n", i * 4, sp[i]);
            shown++;
        }
    }
    if (shown)
        return;
    std::fprintf(stderr, "    (no return addresses in range)\n  recent ICALL targets:");
    for (uint32_t k = 0; k < 16; k++)
        std::fprintf(stderr, " %08X", g_icall_trace[(g_icall_trace_idx + k) & 15]);
    std::fprintf(stderr, "\n");
    for (int i = 0; i < 20; i++)
        std::fprintf(stderr, "    raw[esp+%-4d] 0x%08X\n", i * 4, sp[i]);
}

LONG CALLBACK on_exception(PEXCEPTION_POINTERS ep) {
    if (ep->ExceptionRecord->ExceptionCode != EXCEPTION_ACCESS_VIOLATION)
        return EXCEPTION_CONTINUE_SEARCH;

    const auto fault = ep->ExceptionRecord->ExceptionInformation[1];
    if (fault >= 0xFD000000 && fault < 0xFE000000)
        return EXCEPTION_CONTINUE_SEARCH;
    if (xbox_VehMmioFault(ep))
        return EXCEPTION_CONTINUE_EXECUTION;

    std::fprintf(stderr, "[CRASH] access violation at RIP=0x%llX, %s 0x%llX (guest VA 0x%08X)\n",
                 static_cast<unsigned long long>(ep->ContextRecord->Rip),
                 ep->ExceptionRecord->ExceptionInformation[0] ? "write" : "read",
                 static_cast<unsigned long long>(fault),
                 static_cast<uint32_t>(fault - static_cast<uintptr_t>(g_xbox_mem_offset)));
    std::fprintf(stderr, "  eax=%08X ecx=%08X edx=%08X ebx=%08X esi=%08X edi=%08X esp=%08X\n",
                 g_eax, g_ecx, g_edx, g_ebx, g_esi, g_edi, g_esp);
    print_guest_function(reinterpret_cast<void *>(ep->ContextRecord->Rip));
    print_guest_stack();
    std::fflush(stderr);
    return EXCEPTION_CONTINUE_SEARCH;
}

}  // namespace

void install_crash_handler() {
    // Symbols are loaded now: at fault time the process is already damaged.
    SymSetOptions(SYMOPT_DEFERRED_LOADS | SYMOPT_UNDNAME);
    SymInitialize(GetCurrentProcess(), nullptr, TRUE);
    AddVectoredExceptionHandler(1, on_exception);
}

}  // namespace otogi
