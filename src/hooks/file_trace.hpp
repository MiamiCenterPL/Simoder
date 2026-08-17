#pragma once

#include "reverse/game_build.hpp"

#include <string>

namespace sc13::hooks {

/** Installs the build-gated runtime resource hook and optional discovery instrumentation. */
[[nodiscard]] bool InstallTraceHooks(
    const reverse::BuildFingerprint& fingerprint, std::string& error) noexcept;

/** Removes every game-internal and optional discovery hook installed by this module. */
void UninstallTraceHooks() noexcept;

}  // namespace sc13::hooks
