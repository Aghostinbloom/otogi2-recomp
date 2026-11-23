#pragma once

// Access to guest memory and registers for hand-written overrides. An override
// is a void(void) function like the lifted code: arguments come from the guest
// stack and registers, and it returns through g_eax and g_esp.

#include "host/runtime.hpp"

namespace otogi::guest {

inline uint32_t &mem32(uint32_t va) {
    return *reinterpret_cast<uint32_t *>(g_xbox_mem_offset + static_cast<ptrdiff_t>(va));
}

inline uint8_t *bytes(uint32_t va) {
    return reinterpret_cast<uint8_t *>(g_xbox_mem_offset + static_cast<ptrdiff_t>(va));
}

// Stack argument `index` (0-based) of a function that has just been called.
inline uint32_t arg(int index) { return mem32(g_esp + 4 + 4 * index); }

// Return from a stdcall-style function that pops `arg_bytes` of arguments.
inline void ret(uint32_t arg_bytes = 0) { g_esp += 4 + arg_bytes; }

}  // namespace otogi::guest
