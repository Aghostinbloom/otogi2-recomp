#pragma once

namespace otogi {

// Loads symbols and installs the vectored exception handler that forwards
// trapped hardware registers to their models and reports everything else.
void install_crash_handler();

}  // namespace otogi
