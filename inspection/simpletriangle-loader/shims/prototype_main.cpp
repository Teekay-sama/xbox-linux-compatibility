#include "frame_shim.h"
#include "graphics_shim.h"
#include "startup_shim.h"
#include "triangle.h"

#include <cstdio>
#include <memory>
#include <stdexcept>
#include <string_view>
#include <wrl/client.h>

namespace {
void check(HRESULT result, const char* operation) {
    if (SUCCEEDED(result)) return;
    char message[160];
    std::snprintf(message, sizeof(message), "%s failed (0x%08lx)", operation,
                  static_cast<unsigned long>(result));
    throw std::runtime_error(message);
}

void expect_rejected(HRESULT result, const char* operation) {
    if (SUCCEEDED(result)) throw std::runtime_error(operation);
}

struct StartupScope {
    StartupScope() { check(StStartupInitialize(), "Initialize startup shim"); }
    ~StartupScope() { if (active) StStartupUninitialize(); }
    StartupScope(const StartupScope&) = delete;
    StartupScope& operator=(const StartupScope&) = delete;

    void finish() {
        check(StStartupUninitialize(), "Uninitialize startup shim");
        active = false;
    }

    bool active = true;
};

struct PresentContext {
    StFrameSession* session;
    UINT64 token;
};

void present(void* data, ID3D12CommandQueue* queue, ID3D12Resource* target) {
    const auto& context = *static_cast<const PresentContext*>(data);
    expect_rejected(StFramePresent(context.session, context.token + 1, queue, target),
                    "Graphics shim accepted a wrong frame token");
    check(StFramePresent(context.session, context.token, queue, target), "Present frame through shim");
    expect_rejected(StFramePresent(context.session, context.token, queue, target),
                    "Graphics shim accepted a reused frame token");
}

class DemoWindow {
public:
    DemoWindow() : instance_(GetModuleHandleW(nullptr)) {
        WNDCLASSW window_class{};
        window_class.lpfnWndProc = handle_message;
        window_class.hInstance = instance_;
        window_class.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        window_class.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
        window_class.lpszClassName = class_name;
        if (!RegisterClassW(&window_class)) throw std::runtime_error("Cannot register the triangle window class");

        constexpr DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
        RECT bounds{0, 0, 640, 640};
        if (AdjustWindowRect(&bounds, style, FALSE)) {
            handle_ = CreateWindowExW(0, class_name, L"SimpleTriangle - Windows shim prototype",
                                      style, CW_USEDEFAULT, CW_USEDEFAULT,
                                      bounds.right - bounds.left, bounds.bottom - bounds.top,
                                      nullptr, nullptr, instance_, nullptr);
        }
        if (!handle_) {
            UnregisterClassW(class_name, instance_);
            throw std::runtime_error("Cannot create the triangle window");
        }
    }

    ~DemoWindow() {
        if (IsWindow(handle_)) DestroyWindow(handle_);
        UnregisterClassW(class_name, instance_);
    }

    DemoWindow(const DemoWindow&) = delete;
    DemoWindow& operator=(const DemoWindow&) = delete;

    HWND handle() const { return handle_; }

    void show() {
        ShowWindow(handle_, SW_SHOWNORMAL);
        if (!IsWindowVisible(handle_)) throw std::runtime_error("Triangle window was not shown");
    }

    void run(bool auto_close) {
        if (auto_close && !SetTimer(handle_, close_timer, 1500, nullptr)) {
            throw std::runtime_error("Cannot set the window verification timer");
        }
        MSG message{};
        for (;;) {
            const auto result = GetMessageW(&message, nullptr, 0, 0);
            if (result == -1) throw std::runtime_error("Triangle window message loop failed");
            if (result == 0) return;
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
    }

private:
    static LRESULT CALLBACK handle_message(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
        if (message == WM_CLOSE || (message == WM_KEYDOWN && wparam == VK_ESCAPE) ||
            (message == WM_TIMER && wparam == close_timer)) {
            DestroyWindow(window);
            return 0;
        }
        if (message == WM_DESTROY) {
            PostQuitMessage(0);
            return 0;
        }
        return DefWindowProcW(window, message, wparam, lparam);
    }

    static constexpr wchar_t class_name[] = L"CodexSimpleTriangleShimWindow";
    static constexpr UINT_PTR close_timer = 1;
    HINSTANCE instance_;
    HWND handle_ = nullptr;
};

void run_window(ID3D12Device* device, StFrameSession* session, bool auto_close) {
    DemoWindow window;
    check(StFrameAttachWindow(session, window.handle()), "Attach the window to the graphics shim");
    RECT bounds{};
    if (!GetClientRect(window.handle(), &bounds) || bounds.right <= 0 || bounds.bottom <= 0) {
        throw std::runtime_error("Cannot read the triangle window size");
    }
    window.show();
    check(StFrameSchedule(session), "Schedule window frame");
    PresentContext context{session, 0};
    check(StFrameWait(session, &context.token), "Wait for window frame token");
    const TrianglePresentHook hook{&context, present};
    draw_triangle_window(device, static_cast<unsigned>(bounds.right),
                         static_cast<unsigned>(bounds.bottom), hook);
    UINT64 presented = 0;
    check(StFrameGetCount(session, &presented), "Read window present count");
    if (presented != 1) throw std::runtime_error("Window frame was not presented");
    std::printf("Presented SimpleTriangle to a %ld x %ld window. Press Esc to close.\n",
                bounds.right, bounds.bottom);
    std::fflush(stdout);
    window.run(auto_close);
}
} // namespace

int wmain(int argc, wchar_t** argv) {
    enum class Mode { offscreen, window, window_check };
    Mode mode = Mode::offscreen;
    bool warp = false;
    for (int i = 1; i < argc; ++i) {
        const std::wstring_view argument{argv[i]};
        if (argument == L"--warp" && !warp) {
            warp = true;
        } else if (argument == L"--window" && mode == Mode::offscreen) {
            mode = Mode::window;
        } else if (argument == L"--window-check" && mode == Mode::offscreen) {
            mode = Mode::window_check;
        } else {
            std::fputs("Usage: simpletriangle_shim_test.exe [--warp] [--window | --window-check]\n", stderr);
            return 2;
        }
    }

    try {
        StStartupProfile profile{};
        expect_rejected(StStartupGetProfile(&profile), "Startup profile was accessible before initialization");
        StartupScope startup;
        check(StStartupGetProfile(&profile), "Get startup profile");
        if (profile.width == 0 || profile.height == 0) throw std::runtime_error("Empty startup profile");
        std::printf("Startup initialized. Prototype display profile %u x %u.\n",
                    profile.width, profile.height);

        Microsoft::WRL::ComPtr<ID3D12Device> device;
        DXGI_ADAPTER_DESC1 adapter{};
        check(XboxStyleCreateDevice(warp, device.GetAddressOf(), &adapter), "Create desktop D3D12 device");
        std::printf("Graphics shim adapter: %ls.\n", adapter.Description);

        StFrameSession* session = nullptr;
        check(StFrameCreate(device.Get(), &session), "Create graphics shim frame session");
        std::unique_ptr<StFrameSession, decltype(&StFrameDestroy)> frames(session, StFrameDestroy);
        UINT64 early_token = 0;
        expect_rejected(StFrameWait(session, &early_token), "Graphics shim issued an unscheduled token");
        check(StFrameSetInterval(session, 60), "Set frame interval");

        if (mode != Mode::offscreen) {
            run_window(device.Get(), session, mode == Mode::window_check);
            frames.reset();
            startup.finish();
            return 0;
        }

        Pixels pixels{};
        UINT64 prior_token = 0;
        for (UINT64 frame = 1; frame <= 2; ++frame) {
            check(StFrameSchedule(session), "Schedule frame origin");
            PresentContext context{session, 0};
            check(StFrameWait(session, &context.token), "Wait for frame token");
            if (context.token <= prior_token) throw std::runtime_error("Frame token did not advance");
            const TrianglePresentHook hook{&context, present};
            draw_triangle(device.Get(), pixels, TriangleMode::microsoft_reference, &hook);
            const auto triangle_pixels = verify_triangle(pixels, TriangleMode::microsoft_reference);
            UINT64 presented = 0;
            check(StFrameGetCount(session, &presented), "Read graphics shim present count");
            if (presented != frame) throw std::runtime_error("Incorrect present count");
            std::printf("Frame %llu presented and verified (%zu triangle pixels).\n",
                        static_cast<unsigned long long>(frame), triangle_pixels);
            prior_token = context.token;
        }

        frames.reset();
        startup.finish();
        expect_rejected(StStartupGetProfile(&profile), "Startup profile was accessible after shutdown");
        std::printf("Startup, token rejection, GPU present fence, and shutdown checks passed.\n");

        wchar_t exe_path[32768];
        const auto length = GetModuleFileNameW(nullptr, exe_path, ARRAYSIZE(exe_path));
        if (!length || length >= ARRAYSIZE(exe_path)) throw std::runtime_error("Cannot locate fixture directory");
        const auto output = std::filesystem::path(exe_path).parent_path() /
            (warp ? L"simpletriangle-reference-warp.bmp" : L"simpletriangle-reference-intel.bmp");
        save_triangle(output, pixels);
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "Shim fixture failed: %s\n", error.what());
        return 1;
    }
}
