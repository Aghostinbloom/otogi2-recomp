#pragma once

#include <cstdint>
#include <span>
#include <string_view>

namespace otogi {

struct RuntimeDefault {
    const char *name;
    const char *value;
};

// What the host needs to know about one game. The lifted code is per game, so a
// build carries exactly one profile.
struct GameProfile {
    std::string_view name;
    std::uint32_t title_id;
    std::uint32_t entry_point;
    std::string_view xbe_sha1;
    std::string_view save_folder;  // under %LOCALAPPDATA%
    // xboxrecomp options this game needs; the environment still overrides them.
    std::span<const RuntimeDefault> runtime_defaults;
};

}  // namespace otogi
