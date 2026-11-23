#pragma once

#include "host/profile.hpp"

namespace otogi::games {

inline constexpr GameProfile kOtogi2{
    .name = "Otogi 2: Immortal Warriors (USA)",
    .title_id = 0x46530004,
    .entry_point = 0x001641F7,
    .xbe_sha1 = "314d00c38881e92c9a5cf4e1063336e0ec3a1003",
    .save_folder = "Otogi2",
};

}  // namespace otogi::games
