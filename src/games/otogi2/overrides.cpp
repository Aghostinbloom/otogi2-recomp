// Otogi 2 functions replaced or wrapped by hand.
//
// tools.recomp --exclude-manual reads this file: a line starting with
// "void sub_XXXXXXXX(void)" is a replacement, so the lifter leaves that body out;
// "extern void sub_XXXXXXXX_gen(void);" is a wrapper around the lifted body.
// Keep both forms at column 0 so the scanner sees them.

#include "host/guest.hpp"

#include <cstdlib>
#include <cstring>

using otogi::guest::arg;
using otogi::guest::bytes;
using otogi::guest::mem32;
using otogi::guest::ret;

namespace {

constexpr uint32_t kD3DDevicePtr = 0x001EE1F8;  // XDK 5849 D3D global device pointer

bool g_pad_arrived;
uint8_t g_pad_last[18];
uint32_t g_pad_packet;

}  // namespace

extern "C" {

// D3D BlockOnTime(eax = fence, flags): spins until the GPU semaphore
// *(device+0x30) reaches the fence, and nothing executes the pushbuffer to
// write it. Point the semaphore at the device's own fence counter (device+0x2C)
// so every fence reads as done, here and for every other semaphore reader.
// Re-applied on each call because a device reset rewrites the field.
void sub_001E18E0(void) {
    const uint32_t dev = mem32(kD3DDevicePtr);
    mem32(dev + 0x30) = dev + 0x2C;
    g_eax = mem32(dev + 0x2C);
    ret(4);
}

// Per-frame XMV movie step; returns 1 once the movie has ended. XMV's clock only
// starts once its audio pipeline runs, which it does not yet, so the step waits
// in DirectSound forever. Every movie reports ended unless RECOMP_MOVIES=1.
extern void sub_00135AB0_gen(void);
void sub_00135AB0(void) {
    if (std::getenv("RECOMP_MOVIES"))
        return sub_00135AB0_gen();
    g_eax = 1;
    ret();
}

// XAPI input (XPP section). XAPI's USB stack never enumerates a device on the
// runtime's OHCI model, so the eight entry points the game calls are answered
// from the host input instead. All stdcall. Port 0 only, always present.

// XInitDevices(count, types)
void sub_001F0704(void) {
    g_eax = 0;
    ret(8);
}

// XGetDevices(type) -> bitmask of occupied ports
void sub_001F0709(void) {
    g_eax = 1;
    ret(4);
}

// XGetDeviceChanges(type, *insertions, *removals) -> any change. The game opens
// a pad when it is inserted, so port 0 arrives once, as on hardware.
void sub_001F072B(void) {
    mem32(arg(1)) = g_pad_arrived ? 0 : 1;
    mem32(arg(2)) = 0;
    g_eax = g_pad_arrived ? 0 : 1;
    g_pad_arrived = true;
    ret(12);
}

// XInputOpen(type, port, slot, params) -> handle (port + 1), or 0
void sub_001F0402(void) {
    g_eax = arg(1) == 0 ? 1 : 0;
    ret(16);
}

// XInputClose(handle)
void sub_001F0458(void) {
    g_eax = 0;
    ret(4);
}

// XInputGetCapabilities(handle, *caps). XINPUT_CAPABILITIES is 26 bytes:
// SubType at 0, then the supported bits of each input and output.
void sub_001F0464(void) {
    uint8_t *caps = bytes(arg(1));
    std::memset(caps, 0xFF, 26);
    caps[0] = 1;  // XINPUT_DEVSUBTYPE_GC_GAMEPAD
    caps[1] = caps[2] = caps[3] = 0;
    g_eax = 0;
    ret(8);
}

// XInputGetState(handle, *state): dwPacketNumber, then XINPUT_GAMEPAD (18 bytes).
void sub_001F063C(void) {
    uint8_t report[20] = {};
    usb_gamepad_report(static_cast<int>(arg(0) - 1), report, sizeof report);
    if (std::memcmp(g_pad_last, report + 2, sizeof g_pad_last) != 0) {
        std::memcpy(g_pad_last, report + 2, sizeof g_pad_last);
        g_pad_packet++;
    }
    uint8_t *state = bytes(arg(1));
    std::memcpy(state, &g_pad_packet, 4);
    std::memcpy(state + 4, g_pad_last, sizeof g_pad_last);
    g_eax = 0;
    ret(8);
}

// XInputSetState(handle, *feedback): rumble to the host pad. XINPUT_FEEDBACK is
// a 66-byte header (dwStatus first), then the two motor speeds. Completes at
// once: dwStatus 0, and ERROR_IO_PENDING returned as the XDK does on success.
void sub_001F06AF(void) {
    const uint32_t feedback = arg(1);
    XboxVibration motors;
    std::memcpy(&motors, bytes(feedback + 66), sizeof motors);
    xbox_InputSetState(arg(0) - 1, &motors);
    mem32(feedback) = 0;
    g_eax = 997;
    ret(8);
}

}  // extern "C"
