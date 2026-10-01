#pragma once

#include "core/tgi.hpp"

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace sc13::mods {

using ModId = std::string;
using PatchGeneration = std::uint64_t;

/** Represents the complete observable lifecycle of one discovered mod. */
enum class ModState {
    Discovered,
    Inactive,
    Loading,
    Active,
    Unloading,
    Failed,
    Missing,
    Changed
};

/** Identifies the typed PROP value supported by an M3 property patch. */
enum class PropertyType {
    Float
};

/** Defines one deterministic arithmetic operation over a vanilla-derived value. */
enum class PatchOperation {
    Set,
    Add,
    Subtract,
    Multiply,
    Divide
};

/** Stores validated user-facing identity and descriptive metadata. */
struct ModManifest final {
    ModId id;
    std::string name;
    std::string version;
    std::string author;
    std::string description;
};

/** Defines one typed scalar property operation. */
struct PropertyPatch final {
    std::uint32_t propertyId{};
    PropertyType type{PropertyType::Float};
    PatchOperation operation{PatchOperation::Set};
    float value{};
};

/** Groups deterministic property operations for one exact resource. */
struct ResourcePatchDefinition final {
    core::Tgi target{};
    std::vector<PropertyPatch> properties;
};

/** Owns one validated mod's metadata and declarative patches. */
struct ModDefinition final {
    ModManifest manifest;
    std::filesystem::path directory;
    std::vector<ResourcePatchDefinition> patches;
};

/** Associates a registered patch with its stable owner and activation sequence. */
struct RegisteredPatch final {
    ModId owner;
    ResourcePatchDefinition patch;
    std::uint64_t sequence{};
};

/** Describes a same-resource, same-property conflict between two mods. */
struct PatchConflict final {
    core::Tgi target{};
    std::uint32_t propertyId{};
    ModId earlierOwner;
    ModId laterOwner;
};

/** Provides an immutable UI-facing projection of one mod. */
struct ModUiEntry final {
    ModId id;
    std::string name;
    std::string version;
    std::string author;
    ModState state{ModState::Discovered};
    bool canEnable{};
    bool canDisable{};
    bool changed{};
    std::string diagnostic;
};

/** Returns a stable diagnostic label for a lifecycle state. */
[[nodiscard]] const char* ModStateName(ModState state) noexcept;

/** Returns a stable diagnostic label for a patch operation. */
[[nodiscard]] const char* PatchOperationName(PatchOperation operation) noexcept;

}  // namespace sc13::mods
