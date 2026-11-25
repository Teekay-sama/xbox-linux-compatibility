#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#include <wrl/client.h>
#include "fullscreen_vs.h"

#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <string_view>
#include <vector>

using Microsoft::WRL::ComPtr;
using Color = std::array<float, 4>;

namespace {
constexpr UINT width = 16;
constexpr UINT height = 16;
constexpr DXGI_FORMAT format = DXGI_FORMAT_R32G32B32A32_FLOAT;

void check(HRESULT hr, const char* action) {
    if (FAILED(hr)) {
        char message[256];
        std::snprintf(message, sizeof(message), "%s failed (0x%08lx)", action,
                      static_cast<unsigned long>(hr));
        throw std::runtime_error(message);
    }
}

D3D12_HEAP_PROPERTIES heap(D3D12_HEAP_TYPE type) {
    D3D12_HEAP_PROPERTIES result{};
    result.Type = type;
    result.CreationNodeMask = 1;
    result.VisibleNodeMask = 1;
    return result;
}

void wait(ID3D12Device* device, ID3D12CommandQueue* queue) {
    ComPtr<ID3D12Fence> fence;
    check(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)),
          "CreateFence");
    check(queue->Signal(fence.Get(), 1), "Signal fence");
    if (fence->GetCompletedValue() == 1) return;
    HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!event) throw std::runtime_error("Could not create fence event");
    const auto hr = fence->SetEventOnCompletion(1, event);
    const auto result = SUCCEEDED(hr) ? WaitForSingleObject(event, 30000) : WAIT_FAILED;
    CloseHandle(event);
    check(hr, "SetEventOnCompletion");
    if (result != WAIT_OBJECT_0) throw std::runtime_error("Fence wait failed or timed out");
    check(device->GetDeviceRemovedReason(), "Readback device status");
}

int draw(const std::vector<char>& pixel_shader, const Color& reference, bool pipeline_only) {
    ComPtr<IDXGIFactory4> factory;
    ComPtr<IDXGIAdapter> warp;
    ComPtr<ID3D12Device> device;
    check(CreateDXGIFactory2(0, IID_PPV_ARGS(&factory)), "CreateDXGIFactory2");
    check(factory->EnumWarpAdapter(IID_PPV_ARGS(&warp)), "EnumWarpAdapter");
    check(D3D12CreateDevice(warp.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)),
          "D3D12CreateDevice (WARP)");

    D3D12_ROOT_SIGNATURE_DESC root_desc{};
    root_desc.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;
    ComPtr<ID3DBlob> serialized_root;
    check(D3D12SerializeRootSignature(&root_desc, D3D_ROOT_SIGNATURE_VERSION_1,
                                     &serialized_root, nullptr), "Serialize root signature");
    ComPtr<ID3D12RootSignature> root;
    check(device->CreateRootSignature(0, serialized_root->GetBufferPointer(),
                                      serialized_root->GetBufferSize(), IID_PPV_ARGS(&root)),
          "CreateRootSignature");

    D3D12_GRAPHICS_PIPELINE_STATE_DESC pipeline{};
    pipeline.pRootSignature = root.Get();
    pipeline.VS = {fullscreen_vs, sizeof(fullscreen_vs)};
    pipeline.PS = {pixel_shader.data(), pixel_shader.size()};
    pipeline.BlendState.RenderTarget[0] = {
        FALSE, FALSE, D3D12_BLEND_ONE, D3D12_BLEND_ZERO, D3D12_BLEND_OP_ADD,
        D3D12_BLEND_ONE, D3D12_BLEND_ZERO, D3D12_BLEND_OP_ADD,
        D3D12_LOGIC_OP_NOOP, D3D12_COLOR_WRITE_ENABLE_ALL,
    };
    pipeline.SampleMask = UINT_MAX;
    pipeline.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    pipeline.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    pipeline.RasterizerState.DepthClipEnable = TRUE;
    pipeline.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
    pipeline.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_ALWAYS;
    pipeline.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    pipeline.NumRenderTargets = 1;
    pipeline.RTVFormats[0] = format;
    pipeline.SampleDesc.Count = 1;
    ComPtr<ID3D12PipelineState> state;
    check(device->CreateGraphicsPipelineState(&pipeline, IID_PPV_ARGS(&state)),
          "CreateGraphicsPipelineState");
    if (pipeline_only) {
        std::puts("D3D12 WARP: graphics pipeline created");
        return 0;
    }

    D3D12_DESCRIPTOR_HEAP_DESC rtv_desc{};
    rtv_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    rtv_desc.NumDescriptors = 1;
    ComPtr<ID3D12DescriptorHeap> rtv_heap;
    check(device->CreateDescriptorHeap(&rtv_desc, IID_PPV_ARGS(&rtv_heap)),
          "Create RTV descriptor heap");
    const auto rtv = rtv_heap->GetCPUDescriptorHandleForHeapStart();

    D3D12_RESOURCE_DESC texture{};
    texture.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    texture.Width = width;
    texture.Height = height;
    texture.DepthOrArraySize = 1;
    texture.MipLevels = 1;
    texture.Format = format;
    texture.SampleDesc.Count = 1;
    texture.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    const auto default_heap = heap(D3D12_HEAP_TYPE_DEFAULT);
    ComPtr<ID3D12Resource> target;
    check(device->CreateCommittedResource(&default_heap, D3D12_HEAP_FLAG_NONE, &texture,
                                          D3D12_RESOURCE_STATE_RENDER_TARGET, nullptr,
                                          IID_PPV_ARGS(&target)), "Create render target");
    device->CreateRenderTargetView(target.Get(), nullptr, rtv);

    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
    UINT64 readback_bytes = 0;
    device->GetCopyableFootprints(&texture, 0, 1, 0, &footprint, nullptr, nullptr,
                                  &readback_bytes);
    D3D12_RESOURCE_DESC buffer{};
    buffer.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    buffer.Width = readback_bytes;
    buffer.Height = 1;
    buffer.DepthOrArraySize = 1;
    buffer.MipLevels = 1;
    buffer.SampleDesc.Count = 1;
    buffer.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    const auto readback_heap = heap(D3D12_HEAP_TYPE_READBACK);
    ComPtr<ID3D12Resource> readback;
    check(device->CreateCommittedResource(&readback_heap, D3D12_HEAP_FLAG_NONE, &buffer,
                                          D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                          IID_PPV_ARGS(&readback)), "Create readback buffer");

    ComPtr<ID3D12CommandQueue> queue;
    ComPtr<ID3D12CommandAllocator> allocator;
    ComPtr<ID3D12GraphicsCommandList> commands;
    D3D12_COMMAND_QUEUE_DESC queue_desc{};
    queue_desc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    check(device->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&queue)), "CreateCommandQueue");
    check(device->CreateCommandAllocator(queue_desc.Type, IID_PPV_ARGS(&allocator)),
          "CreateCommandAllocator");
    check(device->CreateCommandList(0, queue_desc.Type, allocator.Get(), state.Get(),
                                    IID_PPV_ARGS(&commands)), "CreateCommandList");
    const D3D12_VIEWPORT viewport{0, 0, float(width), float(height), 0, 1};
    const D3D12_RECT scissors{0, 0, width, height};
    // A distinct finite color makes missed pixels fail even for an all-zero reference.
    const Color clear{12345, -12345, 23456, -23456};
    if (reference == clear) throw std::runtime_error("Reference equals the clear sentinel");
    commands->SetGraphicsRootSignature(root.Get());
    commands->RSSetViewports(1, &viewport);
    commands->RSSetScissorRects(1, &scissors);
    commands->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
    commands->ClearRenderTargetView(rtv, clear.data(), 0, nullptr);
    commands->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    commands->DrawInstanced(3, 1, 0, 0);

    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition = {target.Get(), D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
                          D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE};
    commands->ResourceBarrier(1, &barrier);
    D3D12_TEXTURE_COPY_LOCATION destination{}, source{};
    destination.pResource = readback.Get();
    destination.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    destination.PlacedFootprint = footprint;
    source.pResource = target.Get();
    source.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    commands->CopyTextureRegion(&destination, 0, 0, 0, &source, nullptr);
    check(commands->Close(), "Close command list");
    ID3D12CommandList* lists[] = {commands.Get()};
    queue->ExecuteCommandLists(1, lists);
    wait(device.Get(), queue.Get());

    const D3D12_RANGE read_range{0, static_cast<SIZE_T>(readback_bytes)};
    const D3D12_RANGE write_range{0, 0};
    void* mapped = nullptr;
    check(readback->Map(0, &read_range, &mapped), "Map readback buffer");
    UINT mismatches = 0;
    const auto bytes = static_cast<const std::uint8_t*>(mapped) + footprint.Offset;
    for (UINT y = 0; y < height; ++y) {
        for (UINT x = 0; x < width; ++x) {
            Color actual;
            std::memcpy(actual.data(), bytes + y * footprint.Footprint.RowPitch + x * sizeof(actual),
                        sizeof(actual));
            if (actual != reference && mismatches++ == 0) {
                std::fprintf(stderr, "First mismatch (%u,%u): got (%g, %g, %g, %g)\n",
                             x, y, actual[0], actual[1], actual[2], actual[3]);
            }
        }
    }
    readback->Unmap(0, &write_range);
    std::printf("D3D12 WARP: %u/%u pixels matched (%g, %g, %g, %g) exactly\n",
                width * height - mismatches, width * height,
                reference[0], reference[1], reference[2], reference[3]);
    return mismatches != 0;
}
} // namespace

int wmain(int argc, wchar_t** argv) {
    const bool pipeline_only = argc == 7 && std::wstring_view(argv[6]) == L"--pipeline-only";
    if (argc != 6 && !pipeline_only) {
        std::fprintf(stderr, "Usage: run_pixel.exe <shader.dxil> <r> <g> <b> <a> [--pipeline-only]\n");
        return 2;
    }
    try {
        std::ifstream shader(std::filesystem::path(argv[1]), std::ios::binary);
        if (!shader) throw std::runtime_error("Could not open the pixel shader");
        const std::vector<char> binary((std::istreambuf_iterator<char>(shader)), {});
        if (binary.empty()) throw std::runtime_error("Empty pixel shader file");
        Color reference;
        for (size_t i = 0; i < reference.size(); ++i) {
            wchar_t* end = nullptr;
            reference[i] = std::wcstof(argv[i + 2], &end);
            if (end == argv[i + 2] || *end != L'\0' || !std::isfinite(reference[i])) {
                throw std::runtime_error("Expected four finite floating point numbers");
            }
        }
        return draw(binary, reference, pipeline_only);
    } catch (const std::exception& error) {
        std::fprintf(stderr, "%s\n", error.what());
        return 2;
    }
}
