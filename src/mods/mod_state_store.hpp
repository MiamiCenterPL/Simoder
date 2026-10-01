#pragma once

#include "mods/mod_types.hpp"

#include <filesystem>
#include <span>
#include <string>
#include <vector>

namespace sc13::mods {

/** Loads persisted enabled IDs in deterministic activation order. */
[[nodiscard]] bool LoadEnabledState(
    const std::filesystem::path& path,
    std::vector<ModId>& enabled,
    std::string& error) noexcept;

/** Atomically persists enabled IDs without modifying any mod manifest. */
[[nodiscard]] bool SaveEnabledState(
    const std::filesystem::path& path,
    std::span<const ModId> enabled,
    std::string& error) noexcept;

}  // namespace sc13::mods
