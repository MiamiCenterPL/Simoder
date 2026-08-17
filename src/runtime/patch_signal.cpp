#include "runtime/patch_signal.hpp"

namespace sc13::runtime {
namespace {

SRWLOCK g_signalLock = SRWLOCK_INIT;
HANDLE g_patchAppliedSignal = nullptr;

}  // namespace

bool StartPatchAppliedSignal(std::string& error) noexcept {
    AcquireSRWLockExclusive(&g_signalLock);
    if (g_patchAppliedSignal != nullptr) {
        const bool reset = ResetEvent(g_patchAppliedSignal) != FALSE;
        ReleaseSRWLockExclusive(&g_signalLock);
        if (!reset) {
            error = "ResetEvent failed for the patch-applied signal";
        }
        return reset;
    }
    const std::wstring name = PatchAppliedSignalName(GetCurrentProcessId());
    g_patchAppliedSignal = CreateEventW(nullptr, TRUE, FALSE, name.c_str());
    const DWORD createError = GetLastError();
    const bool created = g_patchAppliedSignal != nullptr;
    ReleaseSRWLockExclusive(&g_signalLock);
    if (!created) {
        error = "CreateEventW failed for the patch-applied signal with error " +
                std::to_string(createError);
    }
    return created;
}

bool SignalPatchApplied() noexcept {
    AcquireSRWLockShared(&g_signalLock);
    const bool signaled =
        g_patchAppliedSignal != nullptr && SetEvent(g_patchAppliedSignal) != FALSE;
    ReleaseSRWLockShared(&g_signalLock);
    return signaled;
}

void StopPatchAppliedSignal() noexcept {
    AcquireSRWLockExclusive(&g_signalLock);
    if (g_patchAppliedSignal != nullptr) {
        CloseHandle(g_patchAppliedSignal);
        g_patchAppliedSignal = nullptr;
    }
    ReleaseSRWLockExclusive(&g_signalLock);
}

}  // namespace sc13::runtime
