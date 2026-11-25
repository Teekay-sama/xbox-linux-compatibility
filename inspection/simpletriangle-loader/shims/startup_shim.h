#pragma once

#include <windows.h>

#ifdef ST_BUILD_STARTUP
#define ST_STARTUP_API __declspec(dllexport)
#else
#define ST_STARTUP_API __declspec(dllimport)
#endif

// Internal fixture contract. The profile is a fixed reference size.
struct StStartupProfile {
    UINT width;
    UINT height;
};

// Each successful initialize call must be balanced by uninitialize.
// An exhausted reference count returns HRESULT_FROM_WIN32(ERROR_ARITHMETIC_OVERFLOW).
extern "C" ST_STARTUP_API HRESULT WINAPI StStartupInitialize() noexcept;

// Returns E_POINTER for a null output, or ERROR_NOT_READY as an HRESULT
// when no initialization is active. A non-null output is zeroed on failure.
extern "C" ST_STARTUP_API HRESULT WINAPI StStartupGetProfile(StStartupProfile* profile) noexcept;

// Returns ERROR_NOT_READY as an HRESULT if called when already uninitialized.
extern "C" ST_STARTUP_API HRESULT WINAPI StStartupUninitialize() noexcept;
