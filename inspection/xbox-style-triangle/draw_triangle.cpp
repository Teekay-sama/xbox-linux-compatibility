#include <d3d12.h>

#include "triangle.h"
#include "simpletriangle_ps.h"
#include "simpletriangle_vs.h"
#include "triangle_copy_point_ps.h"
#include "triangle_ps.h"
#include "triangle_texture_vs.h"
#include "triangle_vs.h"

#include <wrl/client.h>

#include <climits>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <stdexcept>

using Microsoft::WRL::ComPtr;

namespace {
constexpr DXGI_FORMAT color_format = DXGI_FORMAT_R8G8B8A8_UNORM;

struct Vertex {
    float position[2];
    float color[3];
};

constexpr Vertex vertices[] = {
    {{-0.75f, -0.75f}, {1.0f, 0.0f, 0.0f}},
    {{ 0.0f,   0.75f}, {0.0f, 1.0f, 0.0f}},
    {{ 0.75f, -0.75f}, {0.0f, 0.0f, 1.0f}},
};

struct TextureVertex {
    float position[2];
    float uv[2];
};

constexpr TextureVertex texture_vertices[] = {
    {{-0.75f, -0.75f}, {0.0f, 1.0f}},
    {{ 0.0f,   0.75f}, {0.5f, 0.0f}},
    {{ 0.75f, -0.75f}, {1.0f, 1.0f}},
};

struct MicrosoftVertex {
    float position[4];
    float color[4];
};

// Microsoft Xbox-GDK-Samples, SimpleTriangle.cpp, s_vertexData, MIT license.
constexpr MicrosoftVertex microsoft_vertices[] = {
    {{ 0.0f,  0.5f, 0.5f, 1.0f}, {1.0f, 0.0f, 0.0f, 1.0f}},
    {{ 0.5f, -0.5f, 0.5f, 1.0f}, {0.0f, 1.0f, 0.0f, 1.0f}},
    {{-0.5f, -0.5f, 0.5f, 1.0f}, {0.0f, 0.0f, 1.0f, 1.0f}},
};

void require(HRESULT hr, const char* action) {
    if (SUCCEEDED(hr)) return;
    char message[160];
    std::snprintf(message, sizeof(message), "%s failed (0x%08lx)", action,
                  static_cast<unsigned long>(hr));
    throw std::runtime_error(message);
}

ComPtr<ID3D12Resource> make_resource(ID3D12Device* device, const D3D12_RESOURCE_DESC& desc,
                                    D3D12_HEAP_TYPE type, D3D12_RESOURCE_STATES state) {
    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = type;
    heap.CreationNodeMask = heap.VisibleNodeMask = 1;
    ComPtr<ID3D12Resource> resource;
    require(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, state,
                                            nullptr, IID_PPV_ARGS(&resource)),
            "Create committed resource");
    return resource;
}

D3D12_RESOURCE_DESC buffer_desc(UINT64 bytes) {
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    desc.Width = bytes;
    desc.Height = desc.DepthOrArraySize = desc.MipLevels = 1;
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    return desc;
}

D3D12_RESOURCE_DESC image_desc(UINT width, UINT height) {
    D3D12_RESOURCE_DESC image{};
    image.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    image.Width = width;
    image.Height = height;
    image.DepthOrArraySize = image.MipLevels = 1;
    image.Format = color_format;
    image.SampleDesc.Count = 1;
    return image;
}

void transition(ID3D12GraphicsCommandList* commands, ID3D12Resource* resource,
                D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after) {
    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition = {resource, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, before, after};
    commands->ResourceBarrier(1, &barrier);
}

struct TextureBinding {
    ComPtr<ID3D12Resource> texture;
    ComPtr<ID3D12Resource> upload;
    ComPtr<ID3D12DescriptorHeap> descriptors;
};

TextureBinding bind_triangle_texture(ID3D12Device* device, ID3D12GraphicsCommandList* commands) {
    TextureBinding binding;
    const auto image = image_desc(texture_width, texture_height);
    binding.texture = make_resource(device, image, D3D12_HEAP_TYPE_DEFAULT,
                                    D3D12_RESOURCE_STATE_COPY_DEST);

    D3D12_PLACED_SUBRESOURCE_FOOTPRINT layout{};
    UINT64 upload_bytes = 0;
    device->GetCopyableFootprints(&image, 0, 1, 0, &layout, nullptr, nullptr, &upload_bytes);
    binding.upload = make_resource(device, buffer_desc(upload_bytes), D3D12_HEAP_TYPE_UPLOAD,
                                   D3D12_RESOURCE_STATE_GENERIC_READ);
    constexpr UINT texture_stride = texture_width * 4;
    static_assert(sizeof(triangle_texture) == texture_stride * texture_height);
    const auto* source_bytes = reinterpret_cast<const std::uint8_t*>(&triangle_texture);
    const D3D12_RANGE no_reads{0, 0};
    void* mapped = nullptr;
    require(binding.upload->Map(0, &no_reads, &mapped), "Map triangle texture upload");
    auto* upload_data = static_cast<std::uint8_t*>(mapped) + layout.Offset;
    for (UINT row = 0; row < texture_height; ++row) {
        std::memcpy(upload_data + row * layout.Footprint.RowPitch,
                    source_bytes + row * texture_stride, texture_stride);
    }
    const D3D12_RANGE written{static_cast<SIZE_T>(layout.Offset), static_cast<SIZE_T>(upload_bytes)};
    binding.upload->Unmap(0, &written);

    D3D12_TEXTURE_COPY_LOCATION source{}, destination{};
    source.pResource = binding.upload.Get();
    source.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    source.PlacedFootprint = layout;
    destination.pResource = binding.texture.Get();
    destination.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    commands->CopyTextureRegion(&destination, 0, 0, 0, &source, nullptr);
    transition(commands, binding.texture.Get(), D3D12_RESOURCE_STATE_COPY_DEST,
               D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);

    D3D12_DESCRIPTOR_HEAP_DESC heap{};
    heap.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    heap.NumDescriptors = 1;
    heap.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    require(device->CreateDescriptorHeap(&heap, IID_PPV_ARGS(&binding.descriptors)),
            "Create triangle texture descriptor heap");
    device->CreateShaderResourceView(binding.texture.Get(), nullptr,
                                      binding.descriptors->GetCPUDescriptorHandleForHeapStart());
    ID3D12DescriptorHeap* heaps[] = {binding.descriptors.Get()};
    commands->SetDescriptorHeaps(1, heaps);
    commands->SetGraphicsRootDescriptorTable(0, binding.descriptors->GetGPUDescriptorHandleForHeapStart());
    return binding;
}

void wait_for_draw(ID3D12Device* device, ID3D12CommandQueue* queue) {
    ComPtr<ID3D12Fence> fence;
    require(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)), "Create fence");
    require(queue->Signal(fence.Get(), 1), "Signal fence");
    if (fence->GetCompletedValue() < 1) {
        const HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (!event) throw std::runtime_error("Create fence event failed");
        const HRESULT hr = fence->SetEventOnCompletion(1, event);
        const DWORD result = SUCCEEDED(hr) ? WaitForSingleObject(event, 30000) : WAIT_FAILED;
        CloseHandle(event);
        require(hr, "Set fence event");
        if (result != WAIT_OBJECT_0) throw std::runtime_error("Graphics fence wait failed or timed out");
    }
    require(device->GetDeviceRemovedReason(), "Check graphics device after draw");
}
void render_triangle(ID3D12Device* device, UINT width, UINT height, Pixels* pixels,
                     TriangleMode mode, const TrianglePresentHook* present_hook) {
    const bool textured = mode == TriangleMode::textured;
    const bool microsoft = mode == TriangleMode::microsoft_reference;
    D3D12_ROOT_SIGNATURE_DESC root_desc{};
    root_desc.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;
    D3D12_DESCRIPTOR_RANGE srv_range{};
    D3D12_ROOT_PARAMETER srv_table{};
    D3D12_STATIC_SAMPLER_DESC sampler{};
    if (textured) {
        srv_range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        srv_range.NumDescriptors = 1;
        srv_range.BaseShaderRegister = 0;
        srv_table.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        srv_table.DescriptorTable = {1, &srv_range};
        srv_table.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
        sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_POINT;
        sampler.AddressU = sampler.AddressV = sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        sampler.MaxAnisotropy = 1;
        sampler.ComparisonFunc = D3D12_COMPARISON_FUNC_ALWAYS;
        sampler.MaxLOD = D3D12_FLOAT32_MAX;
        sampler.ShaderRegister = 0;
        sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
        root_desc.NumParameters = 1;
        root_desc.pParameters = &srv_table;
        root_desc.NumStaticSamplers = 1;
        root_desc.pStaticSamplers = &sampler;
    }
    ComPtr<ID3DBlob> signature;
    require(D3D12SerializeRootSignature(&root_desc, D3D_ROOT_SIGNATURE_VERSION_1,
                                        &signature, nullptr), "Serialize root signature");
    ComPtr<ID3D12RootSignature> root;
    require(device->CreateRootSignature(0,
        microsoft ? static_cast<const void*>(simpletriangle_vs) : signature->GetBufferPointer(),
        microsoft ? sizeof(simpletriangle_vs) : signature->GetBufferSize(),
        IID_PPV_ARGS(&root)), "Create root signature");

    const D3D12_INPUT_ELEMENT_DESC attributes[] = {
        {"POSITION", 0, DXGI_FORMAT_R32G32_FLOAT, 0, offsetof(Vertex, position),
         D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
        {"COLOR", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, offsetof(Vertex, color),
         D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
    };
    const D3D12_INPUT_ELEMENT_DESC texture_attributes[] = {
        {"POSITION", 0, DXGI_FORMAT_R32G32_FLOAT, 0, offsetof(TextureVertex, position),
         D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
        {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, offsetof(TextureVertex, uv),
         D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
    };
    const D3D12_INPUT_ELEMENT_DESC microsoft_attributes[] = {
        {"SV_Position", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, offsetof(MicrosoftVertex, position),
         D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
        {"COLOR", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, offsetof(MicrosoftVertex, color),
         D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
    };
    D3D12_GRAPHICS_PIPELINE_STATE_DESC pipeline{};
    pipeline.pRootSignature = root.Get();
    pipeline.VS = textured ? D3D12_SHADER_BYTECODE{triangle_texture_vs, sizeof(triangle_texture_vs)}
                           : D3D12_SHADER_BYTECODE{triangle_vs, sizeof(triangle_vs)};
    pipeline.PS = textured ? D3D12_SHADER_BYTECODE{triangle_copy_point_ps, sizeof(triangle_copy_point_ps)}
                           : D3D12_SHADER_BYTECODE{triangle_ps, sizeof(triangle_ps)};
    pipeline.InputLayout = {textured ? texture_attributes : attributes, 2};
    if (microsoft) {
        pipeline.VS = {simpletriangle_vs, sizeof(simpletriangle_vs)};
        pipeline.PS = {simpletriangle_ps, sizeof(simpletriangle_ps)};
        pipeline.InputLayout = {microsoft_attributes, 2};
    }
    pipeline.BlendState.RenderTarget[0] = {
        FALSE, FALSE, D3D12_BLEND_ONE, D3D12_BLEND_ZERO, D3D12_BLEND_OP_ADD,
        D3D12_BLEND_ONE, D3D12_BLEND_ZERO, D3D12_BLEND_OP_ADD,
        D3D12_LOGIC_OP_NOOP, D3D12_COLOR_WRITE_ENABLE_ALL,
    };
    pipeline.SampleMask = UINT_MAX;
    pipeline.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    pipeline.RasterizerState.CullMode = microsoft ? D3D12_CULL_MODE_BACK : D3D12_CULL_MODE_NONE;
    pipeline.RasterizerState.DepthClipEnable = TRUE;
    pipeline.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
    pipeline.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_ALWAYS;
    pipeline.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    pipeline.NumRenderTargets = 1;
    const auto target_format = microsoft ? DXGI_FORMAT_B8G8R8A8_UNORM_SRGB : color_format;
    pipeline.RTVFormats[0] = target_format;
    pipeline.SampleDesc.Count = 1;
    ComPtr<ID3D12PipelineState> state;
    require(device->CreateGraphicsPipelineState(&pipeline, IID_PPV_ARGS(&state)),
            "Create triangle graphics pipeline");

    const UINT vertex_bytes = microsoft ? sizeof(microsoft_vertices)
        : (textured ? sizeof(texture_vertices) : sizeof(vertices));
    const UINT vertex_stride = microsoft ? sizeof(MicrosoftVertex)
        : (textured ? sizeof(TextureVertex) : sizeof(Vertex));
    const void* vertex_source = microsoft ? static_cast<const void*>(microsoft_vertices)
        : (textured ? static_cast<const void*>(texture_vertices) : static_cast<const void*>(vertices));
    const auto vertex_data = make_resource(device, buffer_desc(vertex_bytes),
                                           D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);
    const D3D12_RANGE no_reads{0, 0};
    const D3D12_RANGE vertex_writes{0, vertex_bytes};
    void* mapped = nullptr;
    require(vertex_data->Map(0, &no_reads, &mapped), "Map vertex buffer");
    std::memcpy(mapped, vertex_source, vertex_bytes);
    vertex_data->Unmap(0, &vertex_writes);
    const D3D12_VERTEX_BUFFER_VIEW vertex_view{vertex_data->GetGPUVirtualAddress(),
                                               vertex_bytes, vertex_stride};

    auto texture = image_desc(width, height);
    texture.Format = target_format;
    texture.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    const auto target = make_resource(device, texture, D3D12_HEAP_TYPE_DEFAULT,
                                      D3D12_RESOURCE_STATE_RENDER_TARGET);
    D3D12_DESCRIPTOR_HEAP_DESC heap_desc{};
    heap_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    heap_desc.NumDescriptors = 1;
    ComPtr<ID3D12DescriptorHeap> heap;
    require(device->CreateDescriptorHeap(&heap_desc, IID_PPV_ARGS(&heap)), "Create RTV descriptor heap");
    const auto rtv = heap->GetCPUDescriptorHandleForHeapStart();
    device->CreateRenderTargetView(target.Get(), nullptr, rtv);

    D3D12_PLACED_SUBRESOURCE_FOOTPRINT layout{};
    UINT64 readback_bytes = 0;
    ComPtr<ID3D12Resource> readback;
    if (pixels) {
        device->GetCopyableFootprints(&texture, 0, 1, 0, &layout, nullptr, nullptr, &readback_bytes);
        readback = make_resource(device, buffer_desc(readback_bytes), D3D12_HEAP_TYPE_READBACK,
                                 D3D12_RESOURCE_STATE_COPY_DEST);
    }

    D3D12_COMMAND_QUEUE_DESC queue_desc{};
    queue_desc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    ComPtr<ID3D12CommandQueue> queue;
    ComPtr<ID3D12CommandAllocator> allocator;
    ComPtr<ID3D12GraphicsCommandList> commands;
    require(device->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&queue)), "Create draw queue");
    require(device->CreateCommandAllocator(queue_desc.Type, IID_PPV_ARGS(&allocator)),
            "Create draw allocator");
    require(device->CreateCommandList(0, queue_desc.Type, allocator.Get(), state.Get(),
                                       IID_PPV_ARGS(&commands)), "Create draw list");
    const D3D12_VIEWPORT viewport{0, 0, static_cast<float>(width), static_cast<float>(height), 0, 1};
    const D3D12_RECT scissor{0, 0, static_cast<LONG>(width), static_cast<LONG>(height)};
    const float clear[] = {0.0f, 0.0f, 0.0f, 1.0f};
    commands->SetGraphicsRootSignature(root.Get());
    // The texture, upload, and descriptor heap must survive until the queue fence fires.
    const auto binding = textured ? bind_triangle_texture(device, commands.Get()) : TextureBinding{};
    commands->RSSetViewports(1, &viewport);
    commands->RSSetScissorRects(1, &scissor);
    commands->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
    commands->ClearRenderTargetView(rtv, microsoft ? microsoft_clear.data() : clear, 0, nullptr);
    commands->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    commands->IASetVertexBuffers(0, 1, &vertex_view);
    commands->DrawInstanced(3, 1, 0, 0);

    transition(commands.Get(), target.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET,
               D3D12_RESOURCE_STATE_COPY_SOURCE);
    if (readback) {
        D3D12_TEXTURE_COPY_LOCATION src{}, dst{};
        src.pResource = target.Get();
        src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        dst.pResource = readback.Get();
        dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        dst.PlacedFootprint = layout;
        commands->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    }
    require(commands->Close(), "Close draw list");
    ID3D12CommandList* lists[] = {commands.Get()};
    queue->ExecuteCommandLists(1, lists);
    if (present_hook) {
        present_hook->invoke(present_hook->context, queue.Get(), target.Get());
    } else {
        wait_for_draw(device, queue.Get());
    }

    if (!pixels) return;

    const D3D12_RANGE read_range{0, static_cast<SIZE_T>(readback_bytes)};
    const D3D12_RANGE no_writes{0, 0};
    require(readback->Map(0, &read_range, &mapped), "Map triangle readback");
    const auto* bytes = static_cast<const std::uint8_t*>(mapped) + layout.Offset;
    const auto rgba_stride = static_cast<std::size_t>(width) * 4;
    for (UINT row = 0; row < height; ++row) {
        std::memcpy(pixels->data() + row * rgba_stride, bytes + row * layout.Footprint.RowPitch,
                    rgba_stride);
    }
    readback->Unmap(0, &no_writes);
    if (microsoft) {
        for (std::size_t i = 0; i < pixels->size(); i += 4) {
            const auto blue = (*pixels)[i];
            (*pixels)[i] = (*pixels)[i + 2];
            (*pixels)[i + 2] = blue;
        }
    }
}
} // namespace

void draw_triangle(ID3D12Device* device, Pixels& pixels, TriangleMode mode,
                   const TrianglePresentHook* present_hook) {
    render_triangle(device, triangle_width, triangle_height, &pixels, mode, present_hook);
}

void draw_triangle_window(ID3D12Device* device, unsigned width, unsigned height,
                          const TrianglePresentHook& present_hook) {
    if (!device || !width || !height || width > D3D12_REQ_TEXTURE2D_U_OR_V_DIMENSION ||
        height > D3D12_REQ_TEXTURE2D_U_OR_V_DIMENSION || !present_hook.invoke) {
        throw std::invalid_argument("Window draw requires a device, valid dimensions and a present hook");
    }
    render_triangle(device, width, height, nullptr, TriangleMode::microsoft_reference, &present_hook);
}
