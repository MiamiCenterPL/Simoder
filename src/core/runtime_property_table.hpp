#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

namespace sc13::core {

/** Describes one verified 24-byte entry in SimCity's parsed runtime PROP vector. */
struct RuntimePropertyRecord final {
    std::uint32_t id{};
    std::uint32_t valueBits{};
    std::array<std::uint32_t, 3> auxiliary{};
    std::uint32_t metadata{};
};

static_assert(sizeof(RuntimePropertyRecord) == 24U);

/** Classifies structural validation of a parsed runtime PROP vector. */
enum class RuntimeTableStatus {
    Valid,
    WrongCount,
    NotStrictlySorted
};

/** Classifies a narrowly constrained scalar-float runtime edit. */
enum class RuntimeFloatPatchStatus {
    Applied,
    AlreadyApplied,
    InvalidTable,
    PropertyMissing,
    PropertyNotFloat,
    UnexpectedValue
};

/** Validates exact record count and strictly increasing property identifiers. */
[[nodiscard]] RuntimeTableStatus ValidateRuntimePropertyTable(
    std::span<const RuntimePropertyRecord> properties,
    std::size_t expectedCount) noexcept;

/** Finds a unique record in an already sorted runtime property table. */
[[nodiscard]] const RuntimePropertyRecord* FindRuntimeProperty(
    std::span<const RuntimePropertyRecord> properties,
    std::uint32_t identifier) noexcept;

/** Reads a scalar float only when the runtime metadata encodes PROP type 0x0D. */
[[nodiscard]] std::optional<float> ReadRuntimeFloat(
    std::span<const RuntimePropertyRecord> properties,
    std::uint32_t identifier) noexcept;

/** Replaces one expected float while refusing malformed, missing, or surprising input. */
[[nodiscard]] RuntimeFloatPatchStatus PatchRuntimeFloat(
    std::span<RuntimePropertyRecord> properties,
    std::size_t expectedCount,
    std::uint32_t identifier,
    float expectedValue,
    float replacementValue) noexcept;

}  // namespace sc13::core
