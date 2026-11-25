#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <d3d12.h>
#include <d3d12sdklayers.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

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

#include "fullscreen_texture_vs.h"

using Microsoft::WRL::ComPtr;

namespace {
constexpr UINT screen_width = 16;
constexpr UINT screen_height = 16;
constexpr UINT texture_width = 4;
constexpr UINT texture_height = 4;
constexpr DXGI_FORMAT color_format = DXGI_FORMAT_R32G32B32A32_FLOAT;

using Color = std::array<float, 4>;
using Texels = std::array<Color, texture_width * texture_height>;

void check(HRESULT result, const char* operation) {
    if (SUCCEEDED(result)) return;
    char message[256];
    std::snprintf(message, sizeof(message), "%s failed (0x%08lx)",
                  operation, static_cast<unsigned long>(result));
    throw std::runtime_error(message);
}

Texels make_texels(bool second) {
    Texels texels{};
    for (UINT y = 0; y < texture_height; ++y) {
        for (UINT x = 0; x < texture_width; ++x) {
            const float column = float(x) / 8;
            const float row = float(y) / 8;
            const float parity = float((x + y) % 2) / 4;
            // Alpha is three quarters of a binary16 step above an exact value.
            // Nearest-half would round up, while the native RTZ export rounds down.
            const float alpha = (second ? 0.75f : 0.625f) + 3.0f / 8192.0f;
            texels[y * texture_width + x] = second
                ? Color{0.5f + row, 0.5f + column, 0.75f + parity, alpha}
                : Color{column, row, parity + 0.125f, alpha};
        }
    }
    return texels;
}

ComPtr<ID3D12Device> make_device(bool hardware) {
    ComPtr<ID3D12Debug> debug;
    if (GetEnvironmentVariableW(L"TEXTURE_PROOF_DEBUG", nullptr, 0) &&
        SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debug)))) {
        debug->EnableDebugLayer();
    }
    ComPtr<IDXGIFactory6> factory;
    check(CreateDXGIFactory2(0, IID_PPV_ARGS(&factory)), "Create DXGI factory");
    ComPtr<IDXGIAdapter1> adapter;
    ComPtr<ID3D12Device> device;
    if (!hardware) {
        check(factory->EnumWarpAdapter(IID_PPV_ARGS(&adapter)), "Find WARP adapter");
        check(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0,
                               IID_PPV_ARGS(&device)), "Create WARP device");
    } else {
        for (UINT index = 0;; ++index) {
            const auto result = factory->EnumAdapterByGpuPreference(
                index, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE, IID_PPV_ARGS(&adapter));
            if (result == DXGI_ERROR_NOT_FOUND) break;
            check(result, "Enumerate hardware adapters");
            DXGI_ADAPTER_DESC1 description{};
            check(adapter->GetDesc1(&description), "Read adapter metadata");
            if (description.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) continue;
            if (SUCCEEDED(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0,
                                           IID_PPV_ARGS(&device)))) break;
        }
        if (!device) throw std::runtime_error("No Direct3D 12 hardware adapter available");
    }
    DXGI_ADAPTER_DESC1 description{};
    check(adapter->GetDesc1(&description), "Read selected adapter");
    std::printf("D3D12 %s adapter: %ls (vendor 0x%04x, device 0x%04x)\n",
                hardware ? "hardware" : "WARP", description.Description,
                description.VendorId, description.DeviceId);
    return device;
}

ComPtr<ID3D12RootSignature> make_root(ID3D12Device* device) {
    D3D12_DESCRIPTOR_RANGE range{};
    range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    range.NumDescriptors = 1;
    D3D12_ROOT_PARAMETER parameter{};
    parameter.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    parameter.DescriptorTable = {1, &range};
    parameter.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    D3D12_STATIC_SAMPLER_DESC sampler{};
    sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_POINT;
    sampler.AddressU = sampler.AddressV = sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.MaxAnisotropy = 1;
    sampler.ComparisonFunc = D3D12_COMPARISON_FUNC_ALWAYS;
    sampler.MaxLOD = D3D12_FLOAT32_MAX;
    sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    D3D12_ROOT_SIGNATURE_DESC description{};
    description.NumParameters = 1;
    description.pParameters = &parameter;
    description.NumStaticSamplers = 1;
    description.pStaticSamplers = &sampler;
    description.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;
    ComPtr<ID3DBlob> binary;
    check(D3D12SerializeRootSignature(&description, D3D_ROOT_SIGNATURE_VERSION_1,
                                     &binary, nullptr), "Serialize root signature");
    ComPtr<ID3D12RootSignature> root;
    check(device->CreateRootSignature(0, binary->GetBufferPointer(), binary->GetBufferSize(),
                                      IID_PPV_ARGS(&root)), "Create root signature");
    return root;
}

ComPtr<ID3D12PipelineState> make_pipeline(ID3D12Device* device, ID3D12RootSignature* root,
                                         const std::vector<char>& shader) {
    D3D12_GRAPHICS_PIPELINE_STATE_DESC pipeline{};
    pipeline.pRootSignature = root;
    pipeline.VS = {fullscreen_texture_vs, sizeof(fullscreen_texture_vs)};
    pipeline.PS = {shader.data(), shader.size()};
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
    pipeline.RTVFormats[0] = color_format;
    pipeline.SampleDesc.Count = 1;
    ComPtr<ID3D12PipelineState> state;
    const auto result = device->CreateGraphicsPipelineState(&pipeline, IID_PPV_ARGS(&state));
    if (FAILED(result)) {
        ComPtr<ID3D12InfoQueue> messages;
        if (SUCCEEDED(device->QueryInterface(IID_PPV_ARGS(&messages)))) {
            for (UINT64 index = 0; index < messages->GetNumStoredMessages(); ++index) {
                SIZE_T length = 0;
                if (FAILED(messages->GetMessage(index, nullptr, &length))) continue;
                std::vector<char> storage(length);
                auto message = reinterpret_cast<D3D12_MESSAGE*>(storage.data());
                if (SUCCEEDED(messages->GetMessage(index, message, &length))) {
                    std::fprintf(stderr, "%s\n", message->pDescription);
                }
            }
        }
    }
    check(result, "Create texture sampling pipeline");
    return state;
}

D3D12_RESOURCE_DESC image_description(UINT width, UINT height, D3D12_RESOURCE_FLAGS flags) {
    D3D12_RESOURCE_DESC description{};
    description.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    description.Width = width;
    description.Height = height;
    description.DepthOrArraySize = 1;
    description.MipLevels = 1;
    description.Format = color_format;
    description.SampleDesc.Count = 1;
    description.Flags = flags;
    return description;
}

D3D12_RESOURCE_DESC buffer_description(UINT64 bytes) {
    D3D12_RESOURCE_DESC description{};
    description.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    description.Width = bytes;
    description.Height = 1;
    description.DepthOrArraySize = 1;
    description.MipLevels = 1;
    description.SampleDesc.Count = 1;
    description.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    return description;
}

ComPtr<ID3D12Resource> create_resource(ID3D12Device* device, const D3D12_RESOURCE_DESC& description,
                                       D3D12_HEAP_TYPE heap_type, D3D12_RESOURCE_STATES state) {
    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = heap_type;
    heap.CreationNodeMask = 1;
    heap.VisibleNodeMask = 1;
    ComPtr<ID3D12Resource> resource;
    check(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &description, state,
                                          nullptr, IID_PPV_ARGS(&resource)), "Create resource");
    return resource;
}

struct Footprint {
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT location{};
    UINT64 bytes{};
};

Footprint footprint(ID3D12Device* device, const D3D12_RESOURCE_DESC& description) {
    Footprint result;
    device->GetCopyableFootprints(&description, 0, 1, 0, &result.location,
                                  nullptr, nullptr, &result.bytes);
    return result;
}

D3D12_TEXTURE_COPY_LOCATION buffer_location(ID3D12Resource* buffer, const Footprint& layout) {
    D3D12_TEXTURE_COPY_LOCATION location{};
    location.pResource = buffer;
    location.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    location.PlacedFootprint = layout.location;
    return location;
}

D3D12_TEXTURE_COPY_LOCATION image_location(ID3D12Resource* image) {
    D3D12_TEXTURE_COPY_LOCATION location{};
    location.pResource = image;
    location.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    return location;
}

void transition(ID3D12GraphicsCommandList* commands, ID3D12Resource* resource,
                D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after) {
    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition = {resource, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, before, after};
    commands->ResourceBarrier(1, &barrier);
}

ComPtr<ID3D12Resource> upload_texture(ID3D12Device* device, ID3D12GraphicsCommandList* commands,
                                    ID3D12Resource* image, const Texels& texels) {
    const auto layout = footprint(device, image->GetDesc());
    auto upload = create_resource(device, buffer_description(layout.bytes),
                                  D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);
    const D3D12_RANGE no_read{0, 0};
    void* mapped = nullptr;
    check(upload->Map(0, &no_read, &mapped), "Map texture upload");
    auto bytes = static_cast<std::uint8_t*>(mapped) + layout.location.Offset;
    for (UINT row = 0; row < texture_height; ++row) {
        std::memcpy(bytes + row * layout.location.Footprint.RowPitch,
                    texels.data() + row * texture_width, texture_width * sizeof(Color));
    }
    const D3D12_RANGE written{static_cast<SIZE_T>(layout.location.Offset),
                             static_cast<SIZE_T>(layout.bytes)};
    upload->Unmap(0, &written);
    const auto destination = image_location(image);
    const auto source = buffer_location(upload.Get(), layout);
    commands->CopyTextureRegion(&destination, 0, 0, 0, &source, nullptr);
    transition(commands, image, D3D12_RESOURCE_STATE_COPY_DEST,
               D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    return upload;
}

ComPtr<ID3D12DescriptorHeap> make_descriptor_heap(ID3D12Device* device,
                                                 D3D12_DESCRIPTOR_HEAP_TYPE type, UINT count,
                                                 D3D12_DESCRIPTOR_HEAP_FLAGS flags) {
    D3D12_DESCRIPTOR_HEAP_DESC description{};
    description.Type = type;
    description.NumDescriptors = count;
    description.Flags = flags;
    ComPtr<ID3D12DescriptorHeap> heap;
    check(device->CreateDescriptorHeap(&description, IID_PPV_ARGS(&heap)), "Create descriptor heap");
    return heap;
}

void wait(ID3D12Device* device, ID3D12CommandQueue* queue) {
    ComPtr<ID3D12Fence> fence;
    check(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)), "Create fence");
    check(queue->Signal(fence.Get(), 1), "Signal fence");
    if (fence->GetCompletedValue() != 1) {
        HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (!event) throw std::runtime_error("Could not create fence event");
        const auto schedule = fence->SetEventOnCompletion(1, event);
        const auto waited = SUCCEEDED(schedule) ? WaitForSingleObject(event, 30000) : WAIT_FAILED;
        CloseHandle(event);
        check(schedule, "Schedule fence event");
        if (waited != WAIT_OBJECT_0) throw std::runtime_error("Fence wait failed or timed out");
    }
    check(device->GetDeviceRemovedReason(), "Readback device status");
}

UINT compare(ID3D12Resource* readback, const Footprint& layout, const Texels& texels,
             UINT draw_index) {
    const D3D12_RANGE read_range{0, static_cast<SIZE_T>(layout.bytes)};
    const D3D12_RANGE no_write{0, 0};
    void* mapped = nullptr;
    check(readback->Map(0, &read_range, &mapped), "Map draw readback");
    const auto bytes = static_cast<const std::uint8_t*>(mapped) + layout.location.Offset;
    UINT mismatches = 0;
    for (UINT y = 0; y < screen_height; ++y) {
        for (UINT x = 0; x < screen_width; ++x) {
            Color actual;
            std::memcpy(actual.data(), bytes + y * layout.location.Footprint.RowPitch +
                        x * sizeof(actual), sizeof(actual));
            // Point sampling uses floor(normalized pixel center times texture extent).
            const UINT texel_x = (2 * x + 1) * texture_width / (2 * screen_width);
            const UINT texel_y = (2 * y + 1) * texture_height / (2 * screen_height);
            auto expected = texels[texel_y * texture_width + texel_x];
            // RGB is exactly representable. The alpha fixture lies in [0.5, 1),
            // where consecutive positive binary16 values have spacing 1/2048.
            expected[3] = std::floor(expected[3] * 2048.0f) / 2048.0f;
            if (actual != expected && mismatches++ == 0) {
                std::fprintf(stderr, "Draw %u first mismatch at (%u,%u): "
                             "got (%g, %g, %g, %g), expected (%g, %g, %g, %g)\n",
                             draw_index + 1, x, y, actual[0], actual[1], actual[2], actual[3],
                             expected[0], expected[1], expected[2], expected[3]);
            }
        }
    }
    readback->Unmap(0, &no_write);
    constexpr UINT pixels = screen_width * screen_height;
    std::printf("Draw %u (texture %c): %u/%u pixels matched exactly\n",
                draw_index + 1, 'A' + draw_index, pixels - mismatches, pixels);
    return mismatches;
}

int draw(const std::vector<char>& shader, bool hardware, bool expect_stale, bool pipeline_only) {
    const auto device = make_device(hardware);
    const auto root = make_root(device.Get());
    const auto pipeline = make_pipeline(device.Get(), root.Get(), shader);
    if (pipeline_only) {
        std::printf("D3D12 %s: graphics pipeline created\n", hardware ? "hardware" : "WARP");
        return 0;
    }

    D3D12_COMMAND_QUEUE_DESC queue_desc{};
    queue_desc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    ComPtr<ID3D12CommandQueue> queue;
    ComPtr<ID3D12CommandAllocator> allocator;
    ComPtr<ID3D12GraphicsCommandList> commands;
    check(device->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&queue)), "Create command queue");
    check(device->CreateCommandAllocator(queue_desc.Type, IID_PPV_ARGS(&allocator)),
          "Create command allocator");
    check(device->CreateCommandList(0, queue_desc.Type, allocator.Get(), pipeline.Get(),
                                    IID_PPV_ARGS(&commands)), "Create command list");

    const auto srv_heap = make_descriptor_heap(device.Get(), D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV,
                                              2, D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE);
    const auto rtv_heap = make_descriptor_heap(device.Get(), D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 1,
                                              D3D12_DESCRIPTOR_HEAP_FLAG_NONE);
    const UINT increment = device->GetDescriptorHandleIncrementSize(
        D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    const auto texture_desc = image_description(texture_width, texture_height,
                                                D3D12_RESOURCE_FLAG_NONE);
    const std::array<Texels, 2> texels{make_texels(false), make_texels(true)};
    std::array<ComPtr<ID3D12Resource>, 2> textures;
    std::array<ComPtr<ID3D12Resource>, 2> uploads;
    for (UINT index = 0; index < 2; ++index) {
        textures[index] = create_resource(device.Get(), texture_desc, D3D12_HEAP_TYPE_DEFAULT,
                                          D3D12_RESOURCE_STATE_COPY_DEST);
        uploads[index] = upload_texture(device.Get(), commands.Get(), textures[index].Get(),
                                        texels[index]);
        auto descriptor = srv_heap->GetCPUDescriptorHandleForHeapStart();
        descriptor.ptr += SIZE_T(index) * increment;
        device->CreateShaderResourceView(textures[index].Get(), nullptr, descriptor);
    }

    const auto target_desc = image_description(screen_width, screen_height,
                                               D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET);
    const auto target = create_resource(device.Get(), target_desc, D3D12_HEAP_TYPE_DEFAULT,
                                        D3D12_RESOURCE_STATE_RENDER_TARGET);
    const auto layout = footprint(device.Get(), target_desc);
    const auto rtv = rtv_heap->GetCPUDescriptorHandleForHeapStart();
    device->CreateRenderTargetView(target.Get(), nullptr, rtv);
    const D3D12_VIEWPORT viewport{0, 0, float(screen_width), float(screen_height), 0, 1};
    const D3D12_RECT scissor{0, 0, screen_width, screen_height};
    ID3D12DescriptorHeap* heaps[]{srv_heap.Get()};
    commands->SetDescriptorHeaps(1, heaps);
    commands->SetGraphicsRootSignature(root.Get());
    commands->RSSetViewports(1, &viewport);
    commands->RSSetScissorRects(1, &scissor);
    commands->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
    commands->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

    std::array<ComPtr<ID3D12Resource>, 2> readbacks;
    for (UINT index = 0; index < 2; ++index) {
        if (index) {
            transition(commands.Get(), target.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE,
                       D3D12_RESOURCE_STATE_RENDER_TARGET);
        }
        auto descriptor = srv_heap->GetGPUDescriptorHandleForHeapStart();
        descriptor.ptr += UINT64(expect_stale ? 0 : index) * increment;
        commands->SetGraphicsRootDescriptorTable(0, descriptor);
        const Color sentinel{-2.0f, -3.0f, -4.0f, -5.0f};
        commands->ClearRenderTargetView(rtv, sentinel.data(), 0, nullptr);
        commands->DrawInstanced(3, 1, 0, 0);
        transition(commands.Get(), target.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET,
                   D3D12_RESOURCE_STATE_COPY_SOURCE);
        readbacks[index] = create_resource(device.Get(), buffer_description(layout.bytes),
                                          D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST);
        const auto source = image_location(target.Get());
        const auto destination = buffer_location(readbacks[index].Get(), layout);
        commands->CopyTextureRegion(&destination, 0, 0, 0, &source, nullptr);
    }
    check(commands->Close(), "Close command list");
    ID3D12CommandList* lists[]{commands.Get()};
    queue->ExecuteCommandLists(1, lists);
    wait(device.Get(), queue.Get());

    UINT mismatches = 0;
    for (UINT index = 0; index < 2; ++index) {
        mismatches += compare(readbacks[index].Get(), layout, texels[index], index);
    }
    return mismatches != 0;
}
} // namespace

int wmain(int argc, wchar_t** argv) {
    if (argc < 2 || argc > 4) {
        std::fprintf(stderr, "Usage: run_texture.exe <pixel.dxil> [warp|hardware] "
                             "[--expect-stale|--pipeline-only]\n");
        return 2;
    }
    try {
        const std::wstring_view adapter = argc > 2 ? argv[2] : L"warp";
        if (adapter != L"warp" && adapter != L"hardware") {
            throw std::runtime_error("Adapter must be warp or hardware");
        }
        const std::wstring_view option = argc == 4 ? argv[3] : L"";
        if (argc == 4 && option != L"--expect-stale" && option != L"--pipeline-only") {
            throw std::runtime_error("Unknown option");
        }
        std::ifstream stream(std::filesystem::path(argv[1]), std::ios::binary);
        if (!stream) throw std::runtime_error("Could not open the pixel shader");
        const std::vector<char> shader((std::istreambuf_iterator<char>(stream)), {});
        if (shader.empty()) throw std::runtime_error("Empty pixel shader file");
        return draw(shader, adapter == L"hardware", option == L"--expect-stale",
                    option == L"--pipeline-only");
    } catch (const std::exception& error) {
        std::fprintf(stderr, "%s\n", error.what());
        return 2;
    }
}
