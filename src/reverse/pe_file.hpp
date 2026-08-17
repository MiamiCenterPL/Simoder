#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace sc13::reverse {

/** Describes one section in a validated PE32 file. */
struct PeSection {
    std::string name;
    std::uint32_t virtualAddress{};
    std::uint32_t virtualSize{};
    std::uint32_t rawOffset{};
    std::uint32_t rawSize{};
    std::uint32_t characteristics{};

    /** Returns true when the PE loader marks this section executable. */
    [[nodiscard]] bool isExecutable() const noexcept {
        return (characteristics & 0x20000000U) != 0;
    }
};

/** Records a printable ASCII string and its mapped PE address. */
struct PeStringHit {
    std::string text;
    std::uint32_t fileOffset{};
    std::optional<std::uint32_t> rva;
};

/** Reads a PE32 file for deterministic offline address correlation. */
class PeFile final {
public:
    /** Opens and validates a complete PE32 image from disk. */
    [[nodiscard]] bool Open(const std::filesystem::path& path, std::string& error) noexcept;

    /** Maps a file offset in section raw data to its runtime RVA. */
    [[nodiscard]] std::optional<std::uint32_t> FileOffsetToRva(
        std::uint32_t fileOffset) const noexcept;

    /** Finds printable strings containing at least one case-insensitive needle. */
    [[nodiscard]] std::vector<PeStringHit> FindAsciiStrings(
        std::span<const std::string_view> needles,
        std::size_t minimumLength,
        std::size_t maximumHits) const;

    /** Finds a little-endian 32-bit value in executable section bytes and returns hit RVAs. */
    [[nodiscard]] std::vector<std::uint32_t> FindValueInExecutableSections(
        std::uint32_t value,
        std::size_t maximumHits = 4096) const;

    /** Finds a little-endian 32-bit value in every section and returns hit RVAs. */
    [[nodiscard]] std::vector<std::uint32_t> FindValueInAllSections(
        std::uint32_t value,
        std::size_t maximumHits = 4096) const;

    /** Finds executable E8 rel32 call sites whose decoded destination equals one preferred VA. */
    [[nodiscard]] std::vector<std::uint32_t> FindRelativeCallReferences(
        std::uint32_t targetVa,
        std::size_t maximumHits = 4096) const;

    /** Returns the section that owns an RVA, including virtual-only tail bytes. */
    [[nodiscard]] const PeSection* FindSectionForRva(std::uint32_t rva) const noexcept;

    /** Writes an analysis-only PE copy with one section's raw bytes replaced by a runtime capture. */
    [[nodiscard]] bool WriteWithSectionCapture(
        std::string_view sectionName,
        const std::filesystem::path& capturePath,
        const std::filesystem::path& outputPath,
        std::string& error) const noexcept;

    /** Returns the preferred image base from the PE optional header. */
    [[nodiscard]] std::uint32_t imageBase() const noexcept { return imageBase_; }

    /** Returns the entry-point RVA. */
    [[nodiscard]] std::uint32_t entryPointRva() const noexcept { return entryPointRva_; }

    /** Returns SizeOfImage. */
    [[nodiscard]] std::uint32_t imageSize() const noexcept { return imageSize_; }

    /** Returns the COFF machine value. */
    [[nodiscard]] std::uint16_t machine() const noexcept { return machine_; }

    /** Returns the validated section table. */
    [[nodiscard]] std::span<const PeSection> sections() const noexcept { return sections_; }

private:
    std::filesystem::path path_;
    std::vector<std::byte> bytes_;
    std::vector<PeSection> sections_;
    std::uint32_t imageBase_{};
    std::uint32_t entryPointRva_{};
    std::uint32_t imageSize_{};
    std::uint16_t machine_{};
};

}  // namespace sc13::reverse
