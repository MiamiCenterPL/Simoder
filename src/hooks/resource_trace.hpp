#pragma once

#include "core/tgi.hpp"
#include "reverse/game_build.hpp"

#include <set>
#include <string>

namespace sc13::mods {
class PatchRegistry;
}

namespace sc13::runtime {
class RuntimeResourceCache;
}

namespace sc13::hooks {

/** Binds process-lifetime mod patch services before any resource hook is created. */
void ConfigureResourceRuntime(
    mods::PatchRegistry& registry,
    runtime::RuntimeResourceCache& cache) noexcept;

/** Rebuilds retained live resources from vanilla using the current patch registry. */
[[nodiscard]] bool RefreshRuntimeResources(
    const std::set<core::Tgi>& affected,
    std::string& error) noexcept;

/** Creates the critical deserializer hook and optional discovery hooks without enabling them. */
[[nodiscard]] bool CreateResourceTraceHook(
    const reverse::BuildFingerprint& fingerprint, std::string& error) noexcept;

/** Clears local resource-hook state after the shared MinHook lifecycle is removed. */
void ResetResourceTrace() noexcept;

}  // namespace sc13::hooks
