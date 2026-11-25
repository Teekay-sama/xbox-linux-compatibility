#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>
#include <cwchar>

using Microsoft::WRL::ComPtr;

int main() {
    ComPtr<IDXGIFactory6> factory;
    if (FAILED(CreateDXGIFactory2(0, IID_PPV_ARGS(&factory)))) return 1;
    for (UINT i = 0;; ++i) {
        ComPtr<IDXGIAdapter1> adapter;
        if (FAILED(factory->EnumAdapterByGpuPreference(i, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE, IID_PPV_ARGS(&adapter)))) break;
        DXGI_ADAPTER_DESC1 description{};
        if (FAILED(adapter->GetDesc1(&description)) || (description.Flags & DXGI_ADAPTER_FLAG_SOFTWARE)) continue;
        ComPtr<ID3D12Device> device;
        if (FAILED(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)))) continue;
        D3D12_FEATURE_DATA_SHADER_MODEL shader{D3D_SHADER_MODEL_6_6};
        const auto sm = device->CheckFeatureSupport(D3D12_FEATURE_SHADER_MODEL, &shader, sizeof(shader));
        D3D12_FEATURE_DATA_D3D12_OPTIONS7 options{};
        const auto mesh = device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS7, &options, sizeof(options));
        std::wprintf(L"%ls (0x%04x:0x%04x) SM6.6=%ls, mesh query=0x%08lx, mesh tier=%u\n",
                     description.Description, description.VendorId, description.DeviceId,
                     SUCCEEDED(sm) && shader.HighestShaderModel >= D3D_SHADER_MODEL_6_6 ? L"yes" : L"no",
                     static_cast<unsigned long>(mesh), static_cast<unsigned>(options.MeshShaderTier));
    }
    MEMORYSTATUSEX ram{};
    ram.dwLength = sizeof(ram);
    if (GlobalMemoryStatusEx(&ram)) {
        std::wprintf(L"Physical RAM: %.1f GiB\n", double(ram.ullTotalPhys) / (1024*1024*1024));
    }
}
