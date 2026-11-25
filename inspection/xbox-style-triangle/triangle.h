#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>

struct ID3D12Device;
struct ID3D12CommandQueue;
struct ID3D12Resource;

constexpr unsigned triangle_width = 64;
constexpr unsigned triangle_height = 64;
using Pixels = std::array<std::uint8_t, triangle_width * triangle_height * 4>;

constexpr unsigned texture_width = 4;
constexpr unsigned texture_height = 4;
// RGBA8 palette in texture row order. The off-diagonal colors expose UV swaps.
inline constexpr std::array<std::uint8_t, texture_width * texture_height * 4> triangle_texture{
     32,  43,  66, 255,  243, 181,  71, 255,   40, 165, 159, 255,  242, 234, 218, 255,
    241, 141, 106, 255,   47,  64,  99, 255,  241, 207,  98, 255,   43, 124, 147, 255,
     45, 128, 159, 255,  242, 234, 218, 255,   51,  66, 115, 255,  241, 141, 106, 255,
    241, 207,  98, 255,   40, 165, 159, 255,  243, 181,  71, 255,   32,  43,  66, 255,
};

enum class TriangleMode { textured, vertex_color, microsoft_reference };
// Linear equivalent of the sample reference image's sRGB background byte (72).
inline constexpr std::array<float, 4> microsoft_clear{0.0648033f, 0.0648033f, 0.0648033f, 1.0f};

struct TrianglePresentHook {
    void* context;
    void (*invoke)(void*, ID3D12CommandQueue*, ID3D12Resource*);
};

void draw_triangle(ID3D12Device* device, Pixels& pixels, TriangleMode mode,
                   const TrianglePresentHook* present_hook = nullptr);
// The hook receives the submitted BGRA8 sRGB texture in COPY_SOURCE state.
// It must wait for GPU work to finish before returning.
void draw_triangle_window(ID3D12Device* device, unsigned width, unsigned height,
                          const TrianglePresentHook& present_hook);
std::size_t verify_triangle(const Pixels& pixels, TriangleMode mode);
void save_triangle(const std::filesystem::path& path, const Pixels& pixels);
