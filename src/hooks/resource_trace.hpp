#pragma once

#include "reverse/game_build.hpp"

#include <string>

namespace sc13::hooks {

/** Creates the critical deserializer hook and optional discovery hooks without enabling them. */
[[nodiscard]] bool CreateResourceTraceHook(
    const reverse::BuildFingerprint& fingerprint, std::string& error) noexcept;

/** Clears local resource-hook state after the shared MinHook lifecycle is removed. */
void ResetResourceTrace() noexcept;

}  // namespace sc13::hooks
