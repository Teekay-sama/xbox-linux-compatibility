#include "graphics_shim.h"
#include "triangle.h"

#include <cstdio>
#include <stdexcept>
#include <string_view>
#include <wrl/client.h>

int wmain(int argc, wchar_t** argv) {
    bool warp = false;
    TriangleMode mode = TriangleMode::textured;
    for (int i = 1; i < argc; ++i) {
        const std::wstring_view argument{argv[i]};
        if (argument == L"--warp" && !warp) {
            warp = true;
        } else if (argument == L"--color" && mode == TriangleMode::textured) {
            mode = TriangleMode::vertex_color;
        } else if (argument == L"--microsoft" && mode == TriangleMode::textured) {
            mode = TriangleMode::microsoft_reference;
        } else {
            std::fputs("Usage: xbox_style_triangle.exe [--warp] [--color | --microsoft]\n", stderr);
            return 2;
        }
    }

    try {
        Microsoft::WRL::ComPtr<ID3D12Device> device;
        DXGI_ADAPTER_DESC1 adapter{};
        const auto result = XboxStyleCreateDevice(warp, device.GetAddressOf(), &adapter);
        if (FAILED(result)) {
            std::fprintf(stderr, "Graphics shim could not create the device (0x%08lx).\n",
                         static_cast<unsigned long>(result));
            return 1;
        }
        std::printf("Graphics shim selected %ls (%s, vendor 0x%04x, device 0x%04x).\n",
                    adapter.Description, warp ? "WARP" : "hardware",
                    adapter.VendorId, adapter.DeviceId);

        Pixels pixels{};
        const char* shader = nullptr;
        const wchar_t* output_name = nullptr;
        switch (mode) {
        case TriangleMode::textured:
            shader = "translated PS_CopyPoint";
            output_name = warp ? L"textured-triangle-warp.bmp" : L"textured-triangle-hardware.bmp";
            break;
        case TriangleMode::vertex_color:
            shader = "vertex color";
            output_name = warp ? L"triangle-warp.bmp" : L"triangle-hardware.bmp";
            break;
        case TriangleMode::microsoft_reference:
            shader = "Microsoft SimpleTriangle (desktop reference)";
            output_name = warp ? L"simpletriangle-reference-warp.bmp" : L"simpletriangle-reference-intel.bmp";
            break;
        }
        std::printf("Pixel shader: %s.\n", shader);
        draw_triangle(device.Get(), pixels, mode);
        const auto triangle_pixels = verify_triangle(pixels, mode);

        wchar_t exe_path[32768];
        const auto length = GetModuleFileNameW(nullptr, exe_path, ARRAYSIZE(exe_path));
        if (!length || length >= ARRAYSIZE(exe_path)) {
            throw std::runtime_error("Could not locate the executable directory");
        }
        const auto output = std::filesystem::path(exe_path).parent_path() / output_name;
        save_triangle(output, pixels);
        std::printf("Verified all %u pixels against the CPU reference (%zu triangle, %zu background).\n",
                    triangle_width * triangle_height, triangle_pixels,
                    triangle_width * triangle_height - triangle_pixels);
        std::printf("Rendered frame: %ls\n", output.c_str());
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "Triangle test failed: %s\n", error.what());
        return 1;
    }
}
