#pragma once

#include "host/profile.hpp"

namespace otogi::games {

inline constexpr RuntimeDefault kOtogi2RuntimeDefaults[] = {
    {"RECOMP_ASYNC_IO", "1"},    // the stream loader reads overlapped
    {"RECOMP_VBLANK", "1"},      // D3D waits on vblank interrupts
    {"RECOMP_AC97_READY", "1"},  // DirectSound needs the codec and the APU
    {"RECOMP_HEAP_RECLAIM", "1"},  // freed memory is reused; off, every movie and load leaks
};

inline constexpr GameProfile kOtogi2{
    .name = "Otogi 2: Immortal Warriors (USA)",
    .title_id = 0x46530004,
    .entry_point = 0x001641F7,
    .xbe_sha1 = "314d00c38881e92c9a5cf4e1063336e0ec3a1003",
    .save_folder = "Otogi2",
    .runtime_defaults = kOtogi2RuntimeDefaults,
};

}  // namespace otogi::games
