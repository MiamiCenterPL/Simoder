#pragma once
#include "mods/mod_types.hpp"
#include <filesystem>
#include <span>

namespace sc13::dev {
/** @summary Records one ordered operation and its computed scalar values. */
struct ValidationStep final {
    mods::ModId owner;
    core::Tgi target;
    mods::PropertyPatch property;
    float before{};
    float after{};
    bool hasValues{};
};
/** @summary Exposes all source matches without guessing package load precedence. */
struct ResourceValidation final {
    core::Tgi target;
    std::vector<std::string> packages;
    std::string error;
    std::string resourceSha256;
};
/** @summary Owns a machine-readable validation result for an ordered group of mods. */
struct ValidationReport final {
    bool valid{true};
    bool checkedGame{};
    std::vector<std::string> errors;
    std::vector<ResourceValidation> resources;
    std::vector<ValidationStep> steps;
    std::vector<mods::PatchConflict> conflicts;
};
/** @summary Validates every target using optional read-only package data and runtime arithmetic. */
[[nodiscard]] ValidationReport ValidateMods(
    std::span<const mods::ModDefinition> definitions,
    const std::filesystem::path& gameDirectory = {});
/** @summary Serializes a stable versioned contract used by CLI and integration tests. */
[[nodiscard]] std::string ValidationJson(const ValidationReport& report);
/** @summary Formats validation and operation chains for human inspection. */
[[nodiscard]] std::string ValidationText(const ValidationReport& report);
}
