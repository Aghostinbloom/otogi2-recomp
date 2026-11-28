#pragma once

#include <string_view>

namespace otogi {

// Live sensors, on their own thread. The window title shows the frame rate,
// the worst frame of the last second, slow frames (longer than the
// presentation interval's: a stutter) and audio underruns.
// With RECOMP_PERF (on by default) it also prints, every second, an [AUDIO]
// line (underruns, how far the device kept up with real time), and reports
// a freeze while it happens: no new frame for 200 ms prints [FREEZE] and the
// stacks of every thread, at most every 5 seconds.
void start_monitor(std::string_view title);

}  // namespace otogi
