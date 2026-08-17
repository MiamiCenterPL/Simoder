#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace sc13::memory {

/** Represents one byte in a signature, or a wildcard when value is empty. */
using SignatureByte = std::optional<std::uint8_t>;

/** Stores a validated IDA-style byte signature. */
class Signature final {
public:
    /** Parses tokens such as "8B 45 ?? 89". */
    [[nodiscard]] static std::optional<Signature> Parse(
        std::string_view text, std::string& error) noexcept;

    /** Returns the parsed byte and wildcard sequence. */
    [[nodiscard]] std::span<const SignatureByte> bytes() const noexcept { return bytes_; }

private:
    explicit Signature(std::vector<SignatureByte> bytes) : bytes_(std::move(bytes)) {}

    std::vector<SignatureByte> bytes_;
};

/** Finds every match of a signature in the supplied memory range. */
[[nodiscard]] std::vector<std::size_t> FindAll(
    std::span<const std::byte> haystack, const Signature& signature);

/** Classifies exact, absent, and ambiguous signature resolution. */
enum class UniqueSignatureStatus {
    Matched,
    NotFound,
    Ambiguous
};

/** Contains the only offset when resolution is exact and the observed match count. */
struct UniqueSignatureResult final {
    UniqueSignatureStatus status{UniqueSignatureStatus::NotFound};
    std::size_t offset{};
    std::size_t matchCount{};
};

/** Resolves a signature only when exactly one match exists. */
[[nodiscard]] UniqueSignatureResult FindUnique(
    std::span<const std::byte> haystack, const Signature& signature);

}  // namespace sc13::memory
