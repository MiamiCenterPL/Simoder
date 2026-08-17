#pragma once

#include <array>
#include <cstddef>
#include <filesystem>
#include <span>
#include <string>

namespace sc13::core {

using Sha256Digest = std::array<std::byte, 32>;

/** Computes SHA-256 over an in-memory byte sequence using Windows CNG. */
[[nodiscard]] bool Sha256(
    std::span<const std::byte> data, Sha256Digest& digest, std::string& error) noexcept;

/** Computes SHA-256 over a file using bounded streaming reads. */
[[nodiscard]] bool Sha256File(
    const std::filesystem::path& path, Sha256Digest& digest, std::string& error) noexcept;

/** Converts a SHA-256 digest to uppercase hexadecimal text. */
[[nodiscard]] std::string ToHex(const Sha256Digest& digest);

}  // namespace sc13::core
