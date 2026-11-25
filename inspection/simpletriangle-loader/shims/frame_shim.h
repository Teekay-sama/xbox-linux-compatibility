#pragma once

#include <d3d12.h>

#ifdef ST_BUILD_FRAME
#define ST_FRAME_API __declspec(dllexport)
#else
#define ST_FRAME_API __declspec(dllimport)
#endif

struct StFrameSession;

// Desktop fixture ABI. Use each session from one thread. Scheduling establishes
// ordering for one frame at a time; the interval does not pace the display.
extern "C" {
ST_FRAME_API HRESULT WINAPI StFrameCreate(ID3D12Device* device, StFrameSession** session);
ST_FRAME_API void WINAPI StFrameDestroy(StFrameSession* session);
// Attach or detach a desktop HWND while idle. Attached sessions copy their
// COPY_SOURCE frame to a DXGI swap chain before presenting.
ST_FRAME_API HRESULT WINAPI StFrameAttachWindow(StFrameSession* session, HWND window);
ST_FRAME_API HRESULT WINAPI StFrameSetInterval(StFrameSession* session, UINT refresh_hz);
ST_FRAME_API HRESULT WINAPI StFrameSchedule(StFrameSession* session);
ST_FRAME_API HRESULT WINAPI StFrameWait(StFrameSession* session, UINT64* token);
ST_FRAME_API HRESULT WINAPI StFramePresent(StFrameSession* session, UINT64 token,
                                           ID3D12CommandQueue* queue, ID3D12Resource* target);
ST_FRAME_API HRESULT WINAPI StFrameGetCount(StFrameSession* session, UINT64* count);
}
