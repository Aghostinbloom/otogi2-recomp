#include "host/window.hpp"

#include "render/gl_hle.h"

#include <windows.h>

#include <atomic>
#include <cstdio>
#include <string>
#include <thread>

namespace otogi {
namespace {

std::atomic<HWND> g_window{nullptr};

LRESULT CALLBACK window_proc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
    if (msg == WM_CLOSE) {
        std::fprintf(stderr, "window closed\n");
        std::fflush(stderr);
        ExitProcess(0);
    }
    return DefWindowProcW(hwnd, msg, wparam, lparam);
}

void run_window(std::wstring title) {
    WNDCLASSW wc{};
    wc.lpfnWndProc = window_proc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.hCursor = LoadCursorA(nullptr, IDC_ARROW);
    wc.hbrBackground = static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
    wc.lpszClassName = L"Otogi2Recomp";
    RegisterClassW(&wc);

    // 640x480 at the renderer's default scale of 2; it resizes the window to
    // its own scale once its context exists.
    RECT rect{0, 0, 1280, 960};
    AdjustWindowRect(&rect, WS_OVERLAPPEDWINDOW, FALSE);
    HWND hwnd = CreateWindowExW(0, wc.lpszClassName, title.c_str(), WS_OVERLAPPEDWINDOW | WS_VISIBLE,
                                CW_USEDEFAULT, CW_USEDEFAULT, rect.right - rect.left, rect.bottom - rect.top,
                                nullptr, nullptr, wc.hInstance, nullptr);
    if (!hwnd) {
        std::fprintf(stderr, "cannot create the window (error %lu)\n", GetLastError());
        return;
    }
    g_window = hwnd;

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
}

}  // namespace

void start_window(std::string_view title) {
    std::wstring wide(title.begin(), title.end());
    std::thread(run_window, std::move(wide)).detach();
}

}  // namespace otogi

extern "C" void *gl_hle_host_window(void) {
    return otogi::g_window.load();
}
