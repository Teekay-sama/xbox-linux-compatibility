#ifndef ST_BUILD_FRAME
#define ST_BUILD_FRAME
#endif
#include "frame_shim.h"

#include <cstdint>
#include <dxgi1_4.h>
#include <new>
#include <wrl/client.h>

using Microsoft::WRL::ComPtr;

enum class FramePhase { idle, scheduled, token_issued };

struct WindowSubmission {
    ComPtr<ID3D12CommandAllocator> allocator;
    ComPtr<ID3D12GraphicsCommandList> commands;
    ComPtr<ID3D12Resource> source;
    ComPtr<ID3D12Resource> backbuffer;
    HRESULT present_result = S_OK;
};

struct StFrameSession {
    ComPtr<ID3D12Device> device;
    ComPtr<IUnknown> device_identity;
    ComPtr<ID3D12Fence> fence;
    ComPtr<IUnknown> pending_queue;
    HWND window = nullptr;
    ComPtr<IDXGISwapChain3> swapchain;
    ComPtr<ID3D12CommandQueue> window_queue;
    ComPtr<IUnknown> window_queue_identity;
    WindowSubmission submission;
    FramePhase phase = FramePhase::idle;
    UINT refresh_hz = 0;
    UINT64 next_token = 1;
    UINT64 outstanding_token = 0;
    UINT64 fence_value = 0;
    UINT64 presented = 0;
};

namespace {
constexpr ULONGLONG fence_timeout_ms = 10000;

HRESULT wait_for_queue(StFrameSession* session, ID3D12CommandQueue* queue) {
    // Reserve UINT64_MAX for D3D12's device removal sentinel.
    if (session->fence_value >= UINT64_MAX - 1) {
        return HRESULT_FROM_WIN32(ERROR_ARITHMETIC_OVERFLOW);
    }
    const UINT64 fence_value = ++session->fence_value;
    const HRESULT signal = queue->Signal(session->fence.Get(), fence_value);
    if (FAILED(signal)) return signal;

    const ULONGLONG start = GetTickCount64();
    for (;;) {
        const UINT64 complete = session->fence->GetCompletedValue();
        if (complete == UINT64_MAX) {
            const HRESULT removed = session->device->GetDeviceRemovedReason();
            return FAILED(removed) ? removed : DXGI_ERROR_DEVICE_REMOVED;
        }
        if (complete >= fence_value) return session->device->GetDeviceRemovedReason();
        if (GetTickCount64() - start >= fence_timeout_ms) return HRESULT_FROM_WIN32(ERROR_TIMEOUT);
        Sleep(1);
    }
}

HRESULT ensure_swapchain(StFrameSession* session, ID3D12CommandQueue* queue,
                         IUnknown* queue_identity, const D3D12_RESOURCE_DESC& source) {
    if (source.Width == 0 || source.Width > UINT32_MAX || source.Height == 0 ||
        source.DepthOrArraySize != 1 || source.MipLevels != 1 || source.SampleDesc.Count != 1 ||
        (source.Format != DXGI_FORMAT_B8G8R8A8_UNORM_SRGB &&
         source.Format != DXGI_FORMAT_B8G8R8A8_UNORM)) {
        return E_INVALIDARG;
    }

    if (session->swapchain) {
        if (session->window_queue_identity.Get() != queue_identity) return E_INVALIDARG;
        DXGI_SWAP_CHAIN_DESC1 existing{};
        const HRESULT status = session->swapchain->GetDesc1(&existing);
        if (FAILED(status)) return status;
        return existing.Width == source.Width && existing.Height == source.Height ? S_OK : E_INVALIDARG;
    }

    ComPtr<IDXGIFactory2> factory;
    HRESULT status = CreateDXGIFactory2(0, IID_PPV_ARGS(&factory));
    if (FAILED(status)) return status;
    DXGI_SWAP_CHAIN_DESC1 description{};
    description.Width = static_cast<UINT>(source.Width);
    description.Height = source.Height;
    description.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    description.SampleDesc.Count = 1;
    description.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    description.BufferCount = 2;
    description.Scaling = DXGI_SCALING_STRETCH;
    description.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    description.AlphaMode = DXGI_ALPHA_MODE_UNSPECIFIED;

    ComPtr<IDXGISwapChain1> created;
    status = factory->CreateSwapChainForHwnd(queue, session->window, &description,
                                             nullptr, nullptr, &created);
    if (FAILED(status)) return status;
    status = created.As(&session->swapchain);
    if (FAILED(status)) return status;
    factory->MakeWindowAssociation(session->window, DXGI_MWA_NO_ALT_ENTER);
    session->window_queue = queue;
    session->window_queue_identity = queue_identity;
    return S_OK;
}

void transition_backbuffer(ID3D12GraphicsCommandList* commands, ID3D12Resource* backbuffer,
                           D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after) {
    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition = {backbuffer, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, before, after};
    commands->ResourceBarrier(1, &barrier);
}

HRESULT present_to_window(StFrameSession* session, ID3D12CommandQueue* queue,
                          IUnknown* queue_identity, ID3D12Resource* target) {
    HRESULT status = ensure_swapchain(session, queue, queue_identity, target->GetDesc());
    if (FAILED(status)) return status;

    // A retry after a fence timeout completes the existing submission.
    if (!session->submission.commands) {
        WindowSubmission next;
        next.source = target;
        const UINT index = session->swapchain->GetCurrentBackBufferIndex();
        status = session->swapchain->GetBuffer(index, IID_PPV_ARGS(&next.backbuffer));
        if (FAILED(status)) return status;
        status = session->device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                                                          IID_PPV_ARGS(&next.allocator));
        if (FAILED(status)) return status;
        status = session->device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT,
            next.allocator.Get(), nullptr, IID_PPV_ARGS(&next.commands));
        if (FAILED(status)) return status;

        // Both formats are in the same BGRA8 family. The bytes already encode sRGB.
        transition_backbuffer(next.commands.Get(), next.backbuffer.Get(),
                              D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_COPY_DEST);
        next.commands->CopyResource(next.backbuffer.Get(), target);
        transition_backbuffer(next.commands.Get(), next.backbuffer.Get(),
                              D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PRESENT);
        status = next.commands->Close();
        if (FAILED(status)) return status;
        ID3D12CommandList* lists[] = {next.commands.Get()};
        queue->ExecuteCommandLists(1, lists);
        next.present_result = session->swapchain->Present(1, 0);
        session->submission = next;
    } else if (session->submission.source.Get() != target) {
        return E_INVALIDARG;
    }

    status = wait_for_queue(session, queue);
    if (FAILED(status)) return status;
    const HRESULT present_result = session->submission.present_result;
    session->submission = {};
    return present_result;
}
}

HRESULT WINAPI StFrameCreate(ID3D12Device* device, StFrameSession** session) {
    if (!session) return E_POINTER;
    *session = nullptr;
    if (!device) return E_POINTER;

    auto* candidate = new (std::nothrow) StFrameSession;
    if (!candidate) return E_OUTOFMEMORY;
    candidate->device = device;
    HRESULT status = device->QueryInterface(IID_PPV_ARGS(&candidate->device_identity));
    if (SUCCEEDED(status)) {
        status = device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&candidate->fence));
    }
    if (FAILED(status)) {
        delete candidate;
        return status;
    }
    *session = candidate;
    return S_OK;
}

void WINAPI StFrameDestroy(StFrameSession* session) {
    if (session && session->submission.commands && session->window_queue) {
        wait_for_queue(session, session->window_queue.Get());
    }
    delete session;
}

HRESULT WINAPI StFrameAttachWindow(StFrameSession* session, HWND window) {
    if (!session) return E_POINTER;
    if (session->phase != FramePhase::idle) return E_UNEXPECTED;
    if (session->window == window) return S_OK;
    session->swapchain.Reset();
    session->window_queue.Reset();
    session->window_queue_identity.Reset();
    session->window = window;
    return S_OK;
}

HRESULT WINAPI StFrameSetInterval(StFrameSession* session, UINT refresh_hz) {
    if (!session) return E_POINTER;
    if (!refresh_hz) return E_INVALIDARG;
    if (session->phase != FramePhase::idle) return E_UNEXPECTED;
    session->refresh_hz = refresh_hz;
    return S_OK;
}

HRESULT WINAPI StFrameSchedule(StFrameSession* session) {
    if (!session) return E_POINTER;
    if (!session->refresh_hz || session->phase != FramePhase::idle) return E_UNEXPECTED;
    session->phase = FramePhase::scheduled;
    return S_OK;
}

HRESULT WINAPI StFrameWait(StFrameSession* session, UINT64* token) {
    if (!token) return E_POINTER;
    *token = 0;
    if (!session) return E_POINTER;
    if (session->phase != FramePhase::scheduled) return E_UNEXPECTED;
    if (!session->next_token) return HRESULT_FROM_WIN32(ERROR_ARITHMETIC_OVERFLOW);
    session->outstanding_token = session->next_token++;
    session->phase = FramePhase::token_issued;
    *token = session->outstanding_token;
    return S_OK;
}

HRESULT WINAPI StFramePresent(StFrameSession* session, UINT64 token,
                              ID3D12CommandQueue* queue, ID3D12Resource* target) {
    if (!session || !queue || !target) return E_POINTER;
    if (session->phase != FramePhase::token_issued) return E_UNEXPECTED;
    if (token != session->outstanding_token) return E_INVALIDARG;
    if (queue->GetDesc().Type != D3D12_COMMAND_LIST_TYPE_DIRECT ||
        target->GetDesc().Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D) {
        return E_INVALIDARG;
    }

    ComPtr<IUnknown> queue_device;
    HRESULT status = queue->GetDevice(IID_PPV_ARGS(&queue_device));
    if (FAILED(status)) return status;
    ComPtr<IUnknown> target_device;
    status = target->GetDevice(IID_PPV_ARGS(&target_device));
    if (FAILED(status)) return status;
    if (queue_device.Get() != session->device_identity.Get() ||
        target_device.Get() != session->device_identity.Get()) {
        return E_INVALIDARG;
    }

    ComPtr<IUnknown> queue_identity;
    status = queue->QueryInterface(IID_PPV_ARGS(&queue_identity));
    if (FAILED(status)) return status;
    // A timed-out fence cannot be bypassed by signaling a different queue.
    if (session->pending_queue && session->pending_queue.Get() != queue_identity.Get()) {
        return E_INVALIDARG;
    }
    session->pending_queue = queue_identity;
    status = session->window ? present_to_window(session, queue, queue_identity.Get(), target)
                             : wait_for_queue(session, queue);
    if (FAILED(status)) return status;
    session->pending_queue.Reset();
    ++session->presented;
    session->outstanding_token = 0;
    session->phase = FramePhase::idle;
    return status;
}

HRESULT WINAPI StFrameGetCount(StFrameSession* session, UINT64* count) {
    if (!count) return E_POINTER;
    *count = 0;
    if (!session) return E_POINTER;
    *count = session->presented;
    return S_OK;
}
