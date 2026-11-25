#include "triangle.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <stdexcept>

namespace {

constexpr int width = 64;
constexpr int height = 64;

struct Point {
    int x;
    int y;
};

constexpr int edge(Point a, Point b, Point sample) {
    return (b.x - a.x) * (sample.y - a.y) - (b.y - a.y) * (sample.x - a.x);
}

int srgb_byte(double linear) {
    const double srgb = linear <= 0.0031308 ? 12.92 * linear
        : 1.055 * std::pow(linear, 1.0 / 2.4) - 0.055;
    return static_cast<int>(std::lround(srgb * 255.0));
}

}  // namespace

std::size_t verify_triangle(const Pixels& pixels, TriangleMode mode) {
    const bool textured = mode == TriangleMode::textured;
    const bool microsoft = mode == TriangleMode::microsoft_reference;
    // Double the viewport coordinates to keep pixel centers and the edge tests exact.
    const Point red = microsoft ? Point{64, 32} : Point{16, 112};
    const Point green = microsoft ? Point{96, 96} : Point{64, 16};
    const Point blue = microsoft ? Point{32, 96} : Point{112, 112};
    const int area = edge(red, green, blue);
    const int background = microsoft ? srgb_byte(microsoft_clear[0]) : 0;
    constexpr int tex_width = static_cast<int>(texture_width);
    constexpr int tex_height = static_cast<int>(texture_height);
    std::size_t covered = 0;

    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            const Point center{2 * x + 1, 2 * y + 1};
            const std::array weights{
                edge(green, blue, center), edge(blue, red, center), edge(red, green, center),
            };
            // The horizontal edge lies on an integer row. The other edges have
            // slope +/-2, so no half-integer pixel center lies on an edge.
            const bool inside = weights[0] > 0 && weights[1] > 0 && weights[2] > 0;
            std::array expected{background, background, background, 255};
            Point texel{};
            if (inside) {
                ++covered;
                if (textured) {
                    // With clip w = 1, u and v are the normalized viewport x
                    // and y within the triangle's bounds. Integer division
                    // selects the point-sampled texel at the pixel center.
                    texel.x = std::clamp((center.x - red.x) * tex_width / (blue.x - red.x),
                                         0, tex_width - 1);
                    texel.y = std::clamp((center.y - green.y) * tex_height / (red.y - green.y),
                                         0, tex_height - 1);
                    const auto source = static_cast<std::size_t>(texel.y * tex_width + texel.x) * 4;
                    for (std::size_t channel = 0; channel < expected.size(); ++channel) {
                        expected[channel] = triangle_texture[source + channel];
                    }
                } else {
                    for (std::size_t channel = 0; channel < 3; ++channel) {
                        expected[channel] = microsoft
                            ? srgb_byte(static_cast<double>(weights[channel]) / area)
                            : (255 * weights[channel] + area / 2) / area;
                    }
                }
            }

            const auto offset = static_cast<std::size_t>(y * width + x) * 4;
            for (std::size_t channel = 0; channel < expected.size(); ++channel) {
                // The half-float round trip changes a normalized byte value
                // by less than half an 8-bit output step.
                const int tolerance = inside && channel < 3 ? (microsoft ? 2 : (!textured ? 1 : 0)) : 0;
                if (std::abs(pixels[offset + channel] - expected[channel]) > tolerance) {
                    std::ostringstream message;
                    message << "Pixel (" << x << ',' << y << ") channel " << channel
                            << " is " << static_cast<int>(pixels[offset + channel])
                            << ", expected " << expected[channel]
                            << " (tolerance " << tolerance << ')';
                    if (textured && inside) {
                        message << " from texel (" << texel.x << ',' << texel.y << ')';
                    }
                    throw std::runtime_error(message.str());
                }
            }
        }
    }
    return covered;
}

void save_triangle(const std::filesystem::path& path, const Pixels& pixels) {
    // 24-bit BMP stores rows bottom-up. The 64-pixel rows already align to four bytes.
    constexpr std::size_t row_bytes = width * 3;
    constexpr std::uint32_t image_bytes = row_bytes * height;
    std::array<std::uint8_t, 54> header{};
    const auto put = [&](std::size_t offset, std::uint32_t value, unsigned count) {
        for (unsigned byte = 0; byte < count; ++byte) {
            header[offset + byte] = static_cast<std::uint8_t>(value >> (8 * byte));
        }
    };
    header[0] = 'B';
    header[1] = 'M';
    put(2, static_cast<std::uint32_t>(header.size()) + image_bytes, 4);
    put(10, static_cast<std::uint32_t>(header.size()), 4);
    put(14, 40, 4);  // BITMAPINFOHEADER size
    put(18, width, 4);
    put(22, height, 4);
    put(26, 1, 2);  // color planes
    put(28, 24, 2); // bits per pixel
    put(34, image_bytes, 4);

    std::ofstream file(path, std::ios::binary);
    file.write(reinterpret_cast<const char*>(header.data()), header.size());
    for (int y = height - 1; y >= 0; --y) {
        std::array<std::uint8_t, row_bytes> row{};
        for (int x = 0; x < width; ++x) {
            const auto source = static_cast<std::size_t>(y * width + x) * 4;
            const auto target = static_cast<std::size_t>(x) * 3;
            row[target] = pixels[source + 2];
            row[target + 1] = pixels[source + 1];
            row[target + 2] = pixels[source];
        }
        file.write(reinterpret_cast<const char*>(row.data()), row.size());
    }
    file.close();
    if (!file) throw std::runtime_error("Could not save the triangle bitmap");
}
