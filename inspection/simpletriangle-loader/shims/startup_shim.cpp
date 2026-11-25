#ifndef ST_BUILD_STARTUP
#define ST_BUILD_STARTUP
#endif
#include "startup_shim.h"

#include <limits>

namespace {
SRWLOCK startup_lock = SRWLOCK_INIT;
UINT initialization_count = 0;
constexpr StStartupProfile reference_profile{1920, 1080};
}

HRESULT WINAPI StStartupInitialize() noexcept {
    AcquireSRWLockExclusive(&startup_lock);
    const bool overflow = initialization_count == (std::numeric_limits<UINT>::max)();
    if (!overflow) ++initialization_count;
    ReleaseSRWLockExclusive(&startup_lock);
    return overflow ? HRESULT_FROM_WIN32(ERROR_ARITHMETIC_OVERFLOW) : S_OK;
}

HRESULT WINAPI StStartupGetProfile(StStartupProfile* profile) noexcept {
    if (!profile) return E_POINTER;

    AcquireSRWLockShared(&startup_lock);
    const bool ready = initialization_count != 0;
    *profile = ready ? reference_profile : StStartupProfile{};
    ReleaseSRWLockShared(&startup_lock);
    return ready ? S_OK : HRESULT_FROM_WIN32(ERROR_NOT_READY);
}

HRESULT WINAPI StStartupUninitialize() noexcept {
    AcquireSRWLockExclusive(&startup_lock);
    const bool ready = initialization_count != 0;
    if (ready) --initialization_count;
    ReleaseSRWLockExclusive(&startup_lock);
    return ready ? S_OK : HRESULT_FROM_WIN32(ERROR_NOT_READY);
}
