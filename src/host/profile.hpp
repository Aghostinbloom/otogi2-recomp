#pragma once

#include <cstdint>
#include <string_view>

namespace otogi {

// What the host needs to know about one game. The lifted code is per game, so a
// build carries exactly one profile.
struct GameProfile {
    std::string_view name;
    std::uint32_t title_id;
    std::uint32_t entry_point;
    std::string_view xbe_sha1;
    std::string_view save_folder;  // under %LOCALAPPDATA%
};

}  // namespace otogi
