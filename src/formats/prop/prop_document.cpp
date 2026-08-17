#include "formats/prop/prop_document.hpp"

#include <bit>
#include <cstdio>
#include <limits>

namespace sc13::formats::prop {
namespace {

/** Reads a big-endian 16-bit integer at a validated offset. */
[[nodiscard]] bool ReadBigU16(
    std::span<const std::byte> data, std::size_t offset, std::uint16_t& value) noexcept {
    if (offset > data.size() || data.size() - offset < 2) {
        return false;
    }
    value = static_cast<std::uint16_t>(
        (static_cast<std::uint16_t>(data[offset]) << 8U) |
        static_cast<std::uint16_t>(data[offset + 1]));
    return true;
}

/** Reads a big-endian 32-bit integer at a validated offset. */
[[nodiscard]] bool ReadBigU32(
    std::span<const std::byte> data, std::size_t offset, std::uint32_t& value) noexcept {
    if (offset > data.size() || data.size() - offset < 4) {
        return false;
    }
    value = (static_cast<std::uint32_t>(data[offset]) << 24U) |
            (static_cast<std::uint32_t>(data[offset + 1]) << 16U) |
            (static_cast<std::uint32_t>(data[offset + 2]) << 8U) |
            static_cast<std::uint32_t>(data[offset + 3]);
    return true;
}

/** Writes a big-endian 32-bit integer at a validated offset. */
[[nodiscard]] bool WriteBigU32(
    std::span<std::byte> data, std::size_t offset, std::uint32_t value) noexcept {
    if (offset > data.size() || data.size() - offset < 4) {
        return false;
    }
    data[offset] = static_cast<std::byte>((value >> 24U) & 0xFFU);
    data[offset + 1] = static_cast<std::byte>((value >> 16U) & 0xFFU);
    data[offset + 2] = static_cast<std::byte>((value >> 8U) & 0xFFU);
    data[offset + 3] = static_cast<std::byte>(value & 0xFFU);
    return true;
}

/** Returns a contextual unsupported-type error. */
[[nodiscard]] std::string UnsupportedTypeError(
    std::size_t propertyIndex, std::uint16_t type) {
    char buffer[128]{};
    const int length = std::snprintf(
        buffer, sizeof(buffer), "Unsupported scalar PROP type 0x%02X at property %zu", type,
        propertyIndex);
    return length > 0 ? std::string(buffer, static_cast<std::size_t>(length)) :
                        std::string("Unsupported scalar PROP type");
}

/** Computes the byte size of one scalar property value. */
[[nodiscard]] std::optional<std::size_t> ScalarSize(
    std::span<const std::byte> data,
    std::size_t valueOffset,
    std::uint16_t type,
    std::uint16_t specifier,
    std::string& error) noexcept {
    switch (type) {
        case 0x00: return 4;
        case kTypeBoolean: return 1;
        case kTypeInt32:
        case kTypeUInt32:
        case kTypeFloat: return 4;
        case kTypeKey: return 12;
        case kTypeTexts: return 16;
        case kTypeVector2: return 8;
        case kTypeVector3:
        case kTypeColorRgb: return 12;
        case kTypeVector4:
        case kTypeColorRgba: return 16;
        case kTypeBoundingBox: return 24;
        case kTypeString8: {
            std::uint32_t length = 0;
            if (!ReadBigU32(data, valueOffset, length)) {
                error = "PROP string8 length is truncated";
                return std::nullopt;
            }
            if (length == 0 && (specifier & 0x0100U) != 0) {
                if (!ReadBigU32(data, valueOffset + 1, length)) {
                    error = "PROP binary string8 length is truncated";
                    return std::nullopt;
                }
                return static_cast<std::size_t>(5) + length;
            }
            return static_cast<std::size_t>(4) + length;
        }
        case kTypeString16: {
            std::uint32_t length = 0;
            if (!ReadBigU32(data, valueOffset, length)) {
                error = "PROP string16 length is truncated";
                return std::nullopt;
            }
            if (length > ((std::numeric_limits<std::size_t>::max)() - 4U) / 2U) {
                error = "PROP string16 length overflows the host size type";
                return std::nullopt;
            }
            return static_cast<std::size_t>(4) + static_cast<std::size_t>(length) * 2U;
        }
        case kTypeTransformLegacy:
        case kTypeTransform: {
            std::uint32_t flags = 0;
            if (!ReadBigU32(data, valueOffset, flags)) {
                error = "PROP transform flags are truncated";
                return std::nullopt;
            }
            std::size_t size = 4;
            if ((flags & 0x1U) != 0) {
                size += 4;
            }
            if ((flags & 0x2U) != 0) {
                size += 36;
            }
            if ((flags & 0x4U) != 0) {
                size += 12;
            }
            return size;
        }
        default: return std::nullopt;
    }
}

}  // namespace

bool PropDocument::Parse(std::span<const std::byte> data, std::string& error) noexcept {
    try {
        data_.assign(data.begin(), data.end());
        properties_.clear();
        if (data_.size() < 4) {
            error = "PROP resource is too short for its property count";
            return false;
        }

        std::uint32_t propertyCount = 0;
        if (!ReadBigU32(data_, 0, propertyCount) ||
            propertyCount > (data_.size() - 4U) / 8U) {
            error = "PROP property count cannot fit in the resource";
            return false;
        }

        properties_.reserve(propertyCount);
        std::size_t offset = 4;
        for (std::uint32_t propertyIndex = 0; propertyIndex < propertyCount; ++propertyIndex) {
            PropertyRecord property{};
            property.recordOffset = offset;
            if (!ReadBigU32(data_, offset, property.identifier) ||
                !ReadBigU16(data_, offset + 4U, property.rawType) ||
                !ReadBigU16(data_, offset + 6U, property.specifier)) {
                error = "PROP property header is truncated";
                properties_.clear();
                return false;
            }
            property.type = property.rawType & 0x00FFU;
            offset += 8;

            std::uint16_t normalizedSpecifier = property.specifier;
            if (normalizedSpecifier == 0x80FFU) {
                normalizedSpecifier = static_cast<std::uint16_t>(normalizedSpecifier & ~0x0030U);
            }
            property.isArray = (normalizedSpecifier & 0x0030U) != 0 &&
                               (normalizedSpecifier & 0x0040U) == 0;
            if (property.isArray) {
                if (!ReadBigU32(data_, offset, property.count) ||
                    !ReadBigU32(data_, offset + 4U, property.elementSize)) {
                    error = "PROP array metadata is truncated";
                    properties_.clear();
                    return false;
                }
                offset += 8;
                if (property.count != 0 &&
                    property.elementSize > (std::numeric_limits<std::size_t>::max)() / property.count) {
                    error = "PROP array byte size overflows the host size type";
                    properties_.clear();
                    return false;
                }
                property.valueSize =
                    static_cast<std::size_t>(property.count) * property.elementSize;
            } else {
                std::string scalarError;
                const auto scalarSize = ScalarSize(
                    data_, offset, property.type, property.specifier, scalarError);
                if (!scalarSize.has_value()) {
                    error = scalarError.empty() ?
                                UnsupportedTypeError(propertyIndex, property.type) : scalarError;
                    properties_.clear();
                    return false;
                }
                property.valueSize = scalarSize.value();
                property.elementSize = static_cast<std::uint32_t>(property.valueSize);
            }

            property.valueOffset = offset;
            if (offset > data_.size() || property.valueSize > data_.size() - offset) {
                error = "PROP value extends past the end of the resource";
                properties_.clear();
                return false;
            }
            offset += property.valueSize;
            property.recordSize = offset - property.recordOffset;
            properties_.push_back(property);
        }

        if (offset != data_.size()) {
            error = "PROP parser did not consume the complete resource";
            properties_.clear();
            return false;
        }
        return true;
    } catch (...) {
        error = "PROP parsing failed due to an allocation exception";
        data_.clear();
        properties_.clear();
        return false;
    }
}

const PropertyRecord* PropDocument::Find(std::uint32_t identifier) const noexcept {
    for (const PropertyRecord& property : properties_) {
        if (property.identifier == identifier) {
            return &property;
        }
    }
    return nullptr;
}

std::optional<float> PropDocument::GetFloat(std::uint32_t identifier) const noexcept {
    const PropertyRecord* property = Find(identifier);
    if (property == nullptr || property->type != kTypeFloat || property->isArray ||
        property->valueSize != 4) {
        return std::nullopt;
    }
    std::uint32_t bits = 0;
    if (!ReadBigU32(data_, property->valueOffset, bits)) {
        return std::nullopt;
    }
    return std::bit_cast<float>(bits);
}

bool PropDocument::SetFloat(
    std::uint32_t identifier, float value, std::string& error) noexcept {
    const PropertyRecord* property = Find(identifier);
    if (property == nullptr) {
        error = "Requested PROP identifier was not found";
        return false;
    }
    if (property->type != kTypeFloat || property->isArray || property->valueSize != 4) {
        error = "Requested PROP identifier is not a scalar float";
        return false;
    }
    if (!WriteBigU32(data_, property->valueOffset, std::bit_cast<std::uint32_t>(value))) {
        error = "Validated PROP float offset unexpectedly became invalid";
        return false;
    }
    return true;
}

}  // namespace sc13::formats::prop
