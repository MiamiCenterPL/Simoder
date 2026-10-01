#pragma once

#include <string>

namespace sc13::hooks {

/** Creates safe OutputDebugString capture hooks without enabling MinHook globally. */
[[nodiscard]] bool CreateGameLogCaptureHooks(std::string& error) noexcept;

/** Clears original function pointers after the shared MinHook lifecycle stops. */
void ResetGameLogCapture() noexcept;

}  // namespace sc13::hooks
