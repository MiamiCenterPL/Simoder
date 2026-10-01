#pragma once

#include "mods/mod_types.hpp"

#include <filesystem>
#include <string>

namespace sc13::mods {

/** Loads and validates mod.toon plus overrides.toon into typed C++ structures. */
[[nodiscard]] bool LoadModDefinition(
    const std::filesystem::path& directory,
    ModDefinition& definition,
    std::string& error) noexcept;

/** Validates a stable reverse-domain-like mod identity. */
[[nodiscard]] bool IsValidModId(std::string_view id) noexcept;

}  // namespace sc13::mods
