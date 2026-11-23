#pragma once

#include <string_view>

namespace otogi {

// Opens the game window on its own thread, which runs its message loop.
// Closing the window ends the process.
void start_window(std::string_view title);

}  // namespace otogi
