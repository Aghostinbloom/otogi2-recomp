#pragma once

// The xboxrecomp runtime and the lifted code, as seen from C++. Both are C, and
// their headers carry no extern "C" guards.

#include <windows.h>

#include <cstddef>
#include <cstdint>

extern "C" {
#include "kernel.h"
#include "xbox_memory_layout.h"

typedef void (*recomp_func_t)(void);

recomp_func_t recomp_lookup(uint32_t xbox_va);
int recomp_dispatch_init(void);

typedef struct MCPXAPUState MCPXAPUState;
MCPXAPUState *mcpx_apu_init_standalone(uint8_t *ram_ptr);
extern MCPXAPUState *g_apu_state;

// Thread-local in the runtime: a plain extern would name the TLS template, not
// the calling thread's copy.
extern __declspec(thread) uint32_t g_eax, g_ecx, g_edx, g_esp;
extern __declspec(thread) uint32_t g_ebx, g_esi, g_edi;
extern ptrdiff_t g_xbox_mem_offset;
extern uint32_t g_xbox_code_lo, g_xbox_code_hi;

extern volatile uint32_t g_icall_trace[16];
extern volatile uint32_t g_icall_trace_idx;
extern volatile uint64_t g_icall_count;
}
