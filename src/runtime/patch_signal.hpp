#pragma once

#include <Windows.h>

#include <string>

namespace sc13::runtime {

/** Builds the per-process local event name used to prove that the runtime patch was applied. */
[[nodiscard]] inline std::wstring PatchAppliedSignalName(DWORD processId) {
    return L"Local\\SC13ModLoader-PatchApplied-" + std::to_wstring(processId);
}

/** Creates the unsignaled patch-applied event for the current process. */
[[nodiscard]] bool StartPatchAppliedSignal(std::string& error) noexcept;

/** Signals the current process event after the exact runtime write succeeds. */
[[nodiscard]] bool SignalPatchApplied() noexcept;

/** Closes the current process event during controlled shutdown. */
void StopPatchAppliedSignal() noexcept;

}  // namespace sc13::runtime
