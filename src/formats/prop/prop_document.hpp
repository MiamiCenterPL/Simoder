#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace sc13::formats::prop {

constexpr std::uint16_t kTypeBoolean = 0x01;
constexpr std::uint16_t kTypeInt32 = 0x09;
constexpr std::uint16_t kTypeUInt32 = 0x0A;
constexpr std::uint16_t kTypeFloat = 0x0D;
constexpr std::uint16_t kTypeString8 = 0x12;
constexpr std::uint16_t kTypeString16 = 0x13;
constexpr std::uint16_t kTypeKey = 0x20;
constexpr std::uint16_t kTypeTexts = 0x22;
constexpr std::uint16_t kTypeVector2 = 0x30;
constexpr std::uint16_t kTypeVector3 = 0x31;
constexpr std::uint16_t kTypeColorRgb = 0x32;
constexpr std::uint16_t kTypeVector4 = 0x33;
constexpr std::uint16_t kTypeColorRgba = 0x34;
constexpr std::uint16_t kTypeTransformLegacy = 0x37;
constexpr std::uint16_t kTypeTransform = 0x38;
constexpr std::uint16_t kTypeBoundingBox = 0x39;

/** Describes one property while the document retains its exact original bytes. */
struct PropertyRecord {
    std::uint32_t identifier{};
    std::uint16_t rawType{};
    std::uint16_t type{};
    std::uint16_t specifier{};
    std::uint32_t count{1};
    std::uint32_t elementSize{};
    std::size_t recordOffset{};
    std::size_t valueOffset{};
    std::size_t valueSize{};
    std::size_t recordSize{};
    bool isArray{};
};

/** Parses PROP records while preserving every source byte for lossless serialization. */
class PropDocument final {
public:
    /** Parses a complete uncompressed PROP resource. */
    [[nodiscard]] bool Parse(std::span<const std::byte> data, std::string& error) noexcept;

    /** Finds the first property with the requested identifier. */
    [[nodiscard]] const PropertyRecord* Find(std::uint32_t identifier) const noexcept;

    /** Reads a scalar float property with big-endian decoding. */
    [[nodiscard]] std::optional<float> GetFloat(std::uint32_t identifier) const noexcept;

    /** Replaces a scalar float in the preserved byte buffer using big-endian encoding. */
    [[nodiscard]] bool SetFloat(
        std::uint32_t identifier, float value, std::string& error) noexcept;

    /** Returns the exact source bytes, including any in-place validated edits. */
    [[nodiscard]] std::span<const std::byte> bytes() const noexcept { return data_; }

    /** Returns parsed property metadata. */
    [[nodiscard]] std::span<const PropertyRecord> properties() const noexcept { return properties_; }

private:
    std::vector<std::byte> data_;
    std::vector<PropertyRecord> properties_;
};

}  // namespace sc13::formats::prop
