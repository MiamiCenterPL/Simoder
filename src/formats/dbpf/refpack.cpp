#include "formats/dbpf/refpack.hpp"

#include <cstdint>
#include <limits>

namespace sc13::formats::dbpf {
namespace {

/**
 * Implements the public RefPack command layout documented by OpenSC5 while adding
 * strict input, output, distance, and declared-size validation absent from its reference routine.
 */

/** Copies literal bytes after validating input and output bounds. */
[[nodiscard]] bool CopyLiteral(
    std::span<const std::byte> input,
    std::size_t& inputOffset,
    std::span<std::byte> output,
    std::size_t& outputOffset,
    std::size_t count,
    std::string& error) noexcept {
    if (count > input.size() - inputOffset || count > output.size() - outputOffset) {
        error = "RefPack literal command exceeds the input or declared output size";
        return false;
    }
    for (std::size_t index = 0; index < count; ++index) {
        output[outputOffset++] = input[inputOffset++];
    }
    return true;
}

/** Copies an overlapping back-reference after validating distance and output bounds. */
[[nodiscard]] bool CopyReference(
    std::span<std::byte> output,
    std::size_t& outputOffset,
    std::size_t distance,
    std::size_t count,
    std::string& error) noexcept {
    if (distance == 0 || distance > outputOffset || count > output.size() - outputOffset) {
        error = "RefPack back-reference is outside the produced output";
        return false;
    }
    for (std::size_t index = 0; index < count; ++index) {
        output[outputOffset] = output[outputOffset - distance];
        ++outputOffset;
    }
    return true;
}

/** Reads one byte while preserving a precise truncated-stream error. */
[[nodiscard]] bool ReadByte(
    std::span<const std::byte> input,
    std::size_t& offset,
    std::uint8_t& value,
    std::string& error) noexcept {
    if (offset >= input.size()) {
        error = "RefPack stream ended in the middle of a command";
        return false;
    }
    value = static_cast<std::uint8_t>(input[offset++]);
    return true;
}

}  // namespace

bool DecompressRefPack(
    std::span<const std::byte> input,
    std::size_t expectedSize,
    std::vector<std::byte>& output,
    std::string& error) noexcept {
    try {
        output.clear();
        if (input.size() < 5) {
            error = "RefPack stream is too short for its header";
            return false;
        }

        std::size_t inputOffset = 0;
        std::uint8_t signatureHigh = 0;
        std::uint8_t signatureLow = 0;
        if (!ReadByte(input, inputOffset, signatureHigh, error) ||
            !ReadByte(input, inputOffset, signatureLow, error)) {
            return false;
        }
        const std::uint16_t signature =
            static_cast<std::uint16_t>((signatureHigh << 8U) | signatureLow);
        if ((signature & 0x00FFU) != 0x00FBU) {
            error = "RefPack signature does not end in 0xFB";
            return false;
        }

        if ((signature & 0x0100U) != 0) {
            if (input.size() - inputOffset < 3) {
                error = "RefPack stream is missing its compressed-size field";
                return false;
            }
            inputOffset += 3;
        }

        std::uint8_t sizeHigh = 0;
        std::uint8_t sizeMiddle = 0;
        std::uint8_t sizeLow = 0;
        if (!ReadByte(input, inputOffset, sizeHigh, error) ||
            !ReadByte(input, inputOffset, sizeMiddle, error) ||
            !ReadByte(input, inputOffset, sizeLow, error)) {
            return false;
        }
        const std::size_t declaredSize =
            (static_cast<std::size_t>(sizeHigh) << 16U) |
            (static_cast<std::size_t>(sizeMiddle) << 8U) |
            static_cast<std::size_t>(sizeLow);
        if (declaredSize != expectedSize) {
            error = "RefPack header size does not match the DBPF index memory size";
            return false;
        }

        output.resize(declaredSize);
        std::size_t outputOffset = 0;
        bool stopped = false;
        while (!stopped) {
            std::uint8_t control = 0;
            if (!ReadByte(input, inputOffset, control, error)) {
                output.clear();
                return false;
            }

            std::size_t literalCount = 0;
            std::size_t referenceCount = 0;
            std::size_t distance = 0;
            if (control <= 0x7FU) {
                std::uint8_t second = 0;
                if (!ReadByte(input, inputOffset, second, error)) {
                    output.clear();
                    return false;
                }
                literalCount = control & 0x03U;
                referenceCount = ((control & 0x1CU) >> 2U) + 3U;
                distance = ((control & 0x60U) << 3U) + second + 1U;
            } else if (control <= 0xBFU) {
                std::uint8_t second = 0;
                std::uint8_t third = 0;
                if (!ReadByte(input, inputOffset, second, error) ||
                    !ReadByte(input, inputOffset, third, error)) {
                    output.clear();
                    return false;
                }
                literalCount = second >> 6U;
                referenceCount = (control & 0x3FU) + 4U;
                distance = ((second & 0x3FU) << 8U) + third + 1U;
            } else if (control <= 0xDFU) {
                std::uint8_t second = 0;
                std::uint8_t third = 0;
                std::uint8_t fourth = 0;
                if (!ReadByte(input, inputOffset, second, error) ||
                    !ReadByte(input, inputOffset, third, error) ||
                    !ReadByte(input, inputOffset, fourth, error)) {
                    output.clear();
                    return false;
                }
                literalCount = control & 0x03U;
                referenceCount = ((control & 0x0CU) << 6U) + fourth + 5U;
                distance = ((control & 0x10U) << 12U) +
                           (static_cast<std::size_t>(second) << 8U) + third + 1U;
            } else {
                literalCount = ((control & 0x1FU) << 2U) + 4U;
                if (literalCount > 0x70U) {
                    literalCount = control & 0x03U;
                    stopped = true;
                }
            }

            if (!CopyLiteral(input, inputOffset, output, outputOffset, literalCount, error) ||
                (!stopped && referenceCount != 0 &&
                 !CopyReference(output, outputOffset, distance, referenceCount, error))) {
                output.clear();
                return false;
            }
        }

        if (outputOffset != declaredSize) {
            error = "RefPack stop command was reached before the declared output size";
            output.clear();
            return false;
        }
        return true;
    } catch (...) {
        error = "RefPack decompression failed due to an allocation exception";
        output.clear();
        return false;
    }
}

}  // namespace sc13::formats::dbpf
