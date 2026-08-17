#pragma once

#include "core/tgi.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace sc13::formats::dbpf {

/** Describes one validated DBPF v3 index entry. */
struct DbpfEntry {
    core::Tgi tgi{};
    std::uint32_t instanceHigh{};
    std::uint32_t chunkOffset{};
    std::uint32_t diskSize{};
    std::uint32_t memorySize{};
    std::uint16_t compression{};
    std::uint16_t unknown{};
    bool diskSizeHighBit{};

    /** Returns true for the RefPack marker observed in SimCity packages. */
    [[nodiscard]] bool isCompressed() const noexcept { return compression == 0xFFFFU; }
};

/** Reads SimCity DBPF v3 metadata and individual resources without modifying the package. */
class DbpfPackage final {
public:
    /** Opens and validates a DBPF v3 package index. */
    [[nodiscard]] bool Open(const std::filesystem::path& path, std::string& error) noexcept;

    /** Finds the first exact TGI match in the package index. */
    [[nodiscard]] const DbpfEntry* Find(const core::Tgi& tgi) const noexcept;

    /** Reads and, when necessary, safely decompresses one indexed resource. */
    [[nodiscard]] bool ReadResource(
        const DbpfEntry& entry, std::vector<std::byte>& data, std::string& error) const noexcept;

    /** Returns the package path supplied to Open. */
    [[nodiscard]] const std::filesystem::path& path() const noexcept { return path_; }

    /** Returns all validated index entries. */
    [[nodiscard]] std::span<const DbpfEntry> entries() const noexcept { return entries_; }

    /** Returns the DBPF major version. */
    [[nodiscard]] std::uint32_t majorVersion() const noexcept { return majorVersion_; }

    /** Returns the DBPF minor version. */
    [[nodiscard]] std::uint32_t minorVersion() const noexcept { return minorVersion_; }

    /** Returns the index layout flags. */
    [[nodiscard]] std::uint32_t indexFlags() const noexcept { return indexFlags_; }

private:
    std::filesystem::path path_;
    std::vector<DbpfEntry> entries_;
    std::uint64_t fileSize_{};
    std::uint32_t majorVersion_{};
    std::uint32_t minorVersion_{};
    std::uint32_t indexFlags_{};
};

}  // namespace sc13::formats::dbpf
