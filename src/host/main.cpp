#include "games/otogi2/profile.hpp"
#include "host/crash.hpp"
#include "host/runtime.hpp"
#include "host/window.hpp"
#include "render/gl_hle.h"

#include <shlobj.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <vector>

namespace {

constexpr const otogi::GameProfile &kProfile = otogi::games::kOtogi2;

namespace fs = std::filesystem;

std::optional<std::vector<char>> read_file(const fs::path &path) {
    std::ifstream in(path, std::ios::binary);
    if (!in)
        return std::nullopt;
    return std::vector<char>(std::istreambuf_iterator<char>(in), {});
}

std::optional<uint32_t> xbe_title_id(const std::vector<char> &xbe) {
    auto u32 = [&](size_t off) {
        uint32_t v = 0;
        std::memcpy(&v, xbe.data() + off, sizeof v);
        return v;
    };
    if (xbe.size() < 0x180 || std::memcmp(xbe.data(), "XBEH", 4) != 0)
        return std::nullopt;
    const uint32_t base = u32(0x104);
    const uint32_t cert = u32(0x118) - base;
    if (cert + 0xC > xbe.size())
        return std::nullopt;
    return u32(cert + 0x8);
}

fs::path exe_dir() {
    wchar_t buf[MAX_PATH];
    GetModuleFileNameW(nullptr, buf, MAX_PATH);
    return fs::path(buf).parent_path();
}

fs::path save_dir() {
    PWSTR local = nullptr;
    fs::path dir;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &local)))
        dir = fs::path(local) / kProfile.save_folder;
    CoTaskMemFree(local);
    return dir;
}

void apply_runtime_defaults() {
    for (const auto &d : kProfile.runtime_defaults) {
        if (!std::getenv(d.name))
            _putenv_s(d.name, d.value);
    }
}

int fail(const char *message) {
    std::fprintf(stderr, "error: %s\n", message);
    return 1;
}

}  // namespace

int main(int argc, char **argv) {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::setvbuf(stderr, nullptr, _IONBF, 0);

    const fs::path game = argc > 1 ? fs::path(argv[1]) : exe_dir() / "game";
    std::printf("%.*s\ngame folder: %s\n", static_cast<int>(kProfile.name.size()), kProfile.name.data(),
                game.string().c_str());

    apply_runtime_defaults();
    otogi::install_crash_handler();
    gl_hle_init();
    otogi::start_window(kProfile.name);

    // The runtime keeps pointers into the image, so it lives until exit.
    static auto xbe = read_file(game / "default.xbe");
    if (!xbe)
        return fail("cannot read default.xbe in the game folder (pass the folder as the first argument)");
    if (xbe_title_id(*xbe) != kProfile.title_id)
        return fail("default.xbe is not this game");

    if (!xbox_MemoryLayoutInit(xbe->data(), xbe->size()))
        return fail("cannot map the Xbox address space");
    g_xbox_mem_offset = xbox_GetMemoryOffset();

    if (std::getenv("RECOMP_AC97_READY"))
        g_apu_state = mcpx_apu_init_standalone(static_cast<uint8_t *>(xbox_GetMemoryBase()));
    xbox_OhciInit();  // inert unless RECOMP_USB is set

    xbox_kernel_init();
    xbox_path_init(game.string().c_str(), save_dir().string().c_str());
    xbox_kernel_bridge_init();
    g_esp = XBOX_STACK_TOP;

    const recomp_func_t entry = recomp_lookup(kProfile.entry_point);
    if (!entry)
        return fail("the lifted code has no function at the profile's entry point; re-run the lift");
    if (!recomp_dispatch_init())
        std::fprintf(stderr, "flat dispatch unavailable; indirect calls use the binary search\n");
    xbox_WatchdogStart();

    std::printf("entry 0x%08X, esp 0x%08X\n", kProfile.entry_point, g_esp);
    entry();

    xbox_kernel_shutdown();
    xbox_MemoryLayoutShutdown();
    return 0;
}
