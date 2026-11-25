#pragma once

#include <d3d12.h>
#include <dxgi1_6.h>

#ifdef BUILD_GRAPHICS_SHIM
#define GRAPHICS_SHIM_API __declspec(dllexport)
#else
#define GRAPHICS_SHIM_API __declspec(dllimport)
#endif

// Synthetic import boundary for the test app. The returned device uses
// the Windows Direct3D 12 interfaces.
extern "C" GRAPHICS_SHIM_API HRESULT WINAPI XboxStyleCreateDevice(
    BOOL use_warp, ID3D12Device** device, DXGI_ADAPTER_DESC1* adapter_description);
