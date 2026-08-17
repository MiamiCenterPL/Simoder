#include "core/runtime_property_table.hpp"

#include <algorithm>
#include <bit>

namespace sc13::core {
namespace {

constexpr std::uint32_t kScalarFloatMetadata = 0x000D0000U;

}  // namespace

RuntimeTableStatus ValidateRuntimePropertyTable(
    std::span<const RuntimePropertyRecord> properties,
    std::size_t expectedCount) noexcept {
    if (properties.size() != expectedCount) {
        return RuntimeTableStatus::WrongCount;
    }
    for (std::size_t index = 1; index < properties.size(); ++index) {
        if (properties[index - 1].id >= properties[index].id) {
            return RuntimeTableStatus::NotStrictlySorted;
        }
    }
    return RuntimeTableStatus::Valid;
}

const RuntimePropertyRecord* FindRuntimeProperty(
    std::span<const RuntimePropertyRecord> properties,
    std::uint32_t identifier) noexcept {
    const auto match = std::lower_bound(
        properties.begin(), properties.end(), identifier,
        [](const RuntimePropertyRecord& property, std::uint32_t requested) {
            return property.id < requested;
        });
    return match != properties.end() && match->id == identifier ? &*match : nullptr;
}

std::optional<float> ReadRuntimeFloat(
    std::span<const RuntimePropertyRecord> properties,
    std::uint32_t identifier) noexcept {
    const RuntimePropertyRecord* const property =
        FindRuntimeProperty(properties, identifier);
    if (property == nullptr || property->metadata != kScalarFloatMetadata) {
        return std::nullopt;
    }
    return std::bit_cast<float>(property->valueBits);
}

RuntimeFloatPatchStatus PatchRuntimeFloat(
    std::span<RuntimePropertyRecord> properties,
    std::size_t expectedCount,
    std::uint32_t identifier,
    float expectedValue,
    float replacementValue) noexcept {
    if (ValidateRuntimePropertyTable(properties, expectedCount) !=
        RuntimeTableStatus::Valid) {
        return RuntimeFloatPatchStatus::InvalidTable;
    }
    const auto match = std::lower_bound(
        properties.begin(), properties.end(), identifier,
        [](const RuntimePropertyRecord& property, std::uint32_t requested) {
            return property.id < requested;
        });
    if (match == properties.end() || match->id != identifier) {
        return RuntimeFloatPatchStatus::PropertyMissing;
    }
    RuntimePropertyRecord* const property = &*match;
    if (property->metadata != kScalarFloatMetadata) {
        return RuntimeFloatPatchStatus::PropertyNotFloat;
    }
    const std::uint32_t expectedBits = std::bit_cast<std::uint32_t>(expectedValue);
    const std::uint32_t replacementBits = std::bit_cast<std::uint32_t>(replacementValue);
    if (property->valueBits == replacementBits) {
        return RuntimeFloatPatchStatus::AlreadyApplied;
    }
    if (property->valueBits != expectedBits) {
        return RuntimeFloatPatchStatus::UnexpectedValue;
    }
    property->valueBits = replacementBits;
    return RuntimeFloatPatchStatus::Applied;
}

}  // namespace sc13::core
