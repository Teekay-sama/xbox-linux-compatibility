#define BUILD_GRAPHICS_SHIM
#include "graphics_shim.h"

#include <wrl/client.h>

using Microsoft::WRL::ComPtr;

HRESULT WINAPI XboxStyleCreateDevice(BOOL use_warp, ID3D12Device** device,
                                     DXGI_ADAPTER_DESC1* adapter_description) {
    if (!device || !adapter_description) return E_POINTER;
    *device = nullptr;
    *adapter_description = {};

    ComPtr<IDXGIFactory6> factory;
    auto result = CreateDXGIFactory2(0, IID_PPV_ARGS(&factory));
    if (FAILED(result)) return result;

    if (use_warp) {
        ComPtr<IDXGIAdapter1> adapter;
        result = factory->EnumWarpAdapter(IID_PPV_ARGS(&adapter));
        if (FAILED(result)) return result;
        result = adapter->GetDesc1(adapter_description);
        if (FAILED(result)) return result;
        return D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0,
                                 IID_PPV_ARGS(device));
    }

    for (UINT index = 0;; ++index) {
        ComPtr<IDXGIAdapter1> adapter;
        result = factory->EnumAdapterByGpuPreference(
            index, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE, IID_PPV_ARGS(&adapter));
        if (FAILED(result)) return result;
        DXGI_ADAPTER_DESC1 description{};
        result = adapter->GetDesc1(&description);
        if (FAILED(result)) return result;
        if (description.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) continue;

        result = D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0,
                                   IID_PPV_ARGS(device));
        if (SUCCEEDED(result)) {
            *adapter_description = description;
            return S_OK;
        }
    }
}
