#include "host/monitor.hpp"

#include "host/stacks.hpp"
#include "render/gl_hle.h"

#include <windows.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>

// The toolkit's audio backend (xboxrecomp/src/apu/apu_xaudio2.h).
extern "C" {
int xa2_is_active(void);
unsigned xa2_underruns(void);
uint64_t xa2_samples_played(void);
}

namespace otogi {
namespace {

constexpr double kFreezeMs = 200.0;       // no frame for this long: a freeze
constexpr double kFreezeStacksGapMs = 5000.0;
constexpr double kDeviceRate = 48000.0;   // the backend's output rate

double now_ms() {
    static LARGE_INTEGER f{};
    LARGE_INTEGER t;
    if (!f.QuadPart)
        QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&t);
    return static_cast<double>(t.QuadPart) * 1000.0 / static_cast<double>(f.QuadPart);
}

bool option_on(const char *name) {
    const char *v = std::getenv(name);
    return v && std::strcmp(v, "0") != 0;
}

void set_title(const std::wstring &text) {
    HWND hwnd = static_cast<HWND>(gl_hle_host_window());
    if (!hwnd)
        return;
    // A timeout, so a busy window thread never stalls the monitor.
    SendMessageTimeoutW(hwnd, WM_SETTEXT, 0, reinterpret_cast<LPARAM>(text.c_str()), SMTO_ABORTIFHUNG, 100,
                        nullptr);
}

void run(std::wstring base) {
    const bool log = option_on("RECOMP_PERF");
    double next_second = now_ms() + 1000.0, last_stacks = 0.0, freeze_since = 0.0;
    double audio_t0 = 0.0;
    uint64_t played0 = 0;
    unsigned underruns0 = 0, underruns_per_s = 0;

    for (;;) {
        Sleep(10);
        const double now = now_ms();

        // A freeze, seen while it lasts: the renderer has not presented for
        // kFreezeMs. Its stacks show where the game is stuck.
        const double last = gl_hle_last_present_ms();
        if (log && gl_hle_presents() && last > 0.0) {
            if (now - last > kFreezeMs && freeze_since != last) {
                freeze_since = last;
                std::fprintf(stderr, "[FREEZE] no frame for %.0f ms after frame %u\n", now - last,
                             gl_hle_presents());
                if (now - last_stacks > kFreezeStacksGapMs) {
                    last_stacks = now;
                    dump_stacks_now();
                }
            } else if (freeze_since > 0.0 && last != freeze_since) {
                std::fprintf(stderr, "[FREEZE] over: %.0f ms without a frame\n", last - freeze_since);
                freeze_since = 0.0;
            }
        }

        if (now < next_second)
            continue;
        next_second += 1000.0;
        if (now > next_second)
            next_second = now + 1000.0;

        if (xa2_is_active()) {
            const unsigned underruns = xa2_underruns();
            const uint64_t played = xa2_samples_played();
            if (audio_t0 > 0.0) {
                const double kept_up = static_cast<double>(played - played0) / ((now - audio_t0) / 1000.0) / kDeviceRate;
                underruns_per_s = underruns - underruns0;
                if (log)
                    std::fprintf(stderr, "[AUDIO] %u underruns; the device played %.1f%% of real time\n",
                                 underruns_per_s, kept_up * 100.0);
            }
            audio_t0 = now;
            played0 = played;
            underruns0 = underruns;
        }

        GlHlePerf p;
        if (gl_hle_perf(&p)) {
            wchar_t text[256];
            swprintf(text, sizeof text / sizeof text[0],
                     L"%ls  |  %.1f fps  |  worst %.0f ms  |  %u over 50 ms  |  audio underruns %u/s", base.c_str(),
                     p.fps, p.worst_ms, p.over50, underruns_per_s);
            set_title(text);
        }
    }
}

}  // namespace

void start_monitor(std::string_view title) {
    std::wstring wide(title.begin(), title.end());
    std::thread(run, std::move(wide)).detach();
}

}  // namespace otogi
