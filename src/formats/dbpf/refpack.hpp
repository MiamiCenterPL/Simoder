#pragma once

#include <cstddef>
#include <span>
#include <string>
#include <vector>

namespace sc13::formats::dbpf {

/** Decompresses a bounds-checked SimCity RefPack stream. */
[[nodiscard]] bool DecompressRefPack(
    std::span<const std::byte> input,
    std::size_t expectedSize,
    std::vector<std::byte>& output,
    std::string& error) noexcept;

}  // namespace sc13::formats::dbpf
