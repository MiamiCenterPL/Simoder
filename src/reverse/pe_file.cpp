#include "reverse/pe_file.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstring>
#include <fstream>
#include <limits>

namespace sc13::reverse {
namespace {

constexpr std::uint16_t kPe32Magic = 0x010BU;
constexpr std::uint16_t kI386Machine = 0x014CU;
constexpr std::uint64_t kMaximumPeFileSize = 1024ULL * 1024ULL * 1024ULL;

/** Reads a little-endian 16-bit integer from a bounded byte vector. */
[[nodiscard]] bool ReadU16(
    std::span<const std::byte> bytes, std::size_t offset, std::uint16_t& value) noexcept {
    if (offset > bytes.size() || bytes.size() - offset < 2) {
        return false;
    }
    value = static_cast<std::uint16_t>(
        static_cast<std::uint16_t>(bytes[offset]) |
        (static_cast<std::uint16_t>(bytes[offset + 1]) << 8U));
    return true;
}

/** Reads a little-endian 32-bit integer from a bounded byte vector. */
[[nodiscard]] bool ReadU32(
    std::span<const std::byte> bytes, std::size_t offset, std::uint32_t& value) noexcept {
    if (offset > bytes.size() || bytes.size() - offset < 4) {
        return false;
    }
    value = static_cast<std::uint32_t>(bytes[offset]) |
            (static_cast<std::uint32_t>(bytes[offset + 1]) << 8U) |
            (static_cast<std::uint32_t>(bytes[offset + 2]) << 16U) |
            (static_cast<std::uint32_t>(bytes[offset + 3]) << 24U);
    return true;
}

/** Returns a lowercase ASCII copy for deterministic substring matching. */
[[nodiscard]] std::string LowerAscii(std::string_view text) {
    std::string result(text);
    std::transform(result.begin(), result.end(), result.begin(), [](unsigned char character) {
        return static_cast<char>(std::tolower(character));
    });
    return result;
}

/** Returns true for characters suitable for a diagnostic ASCII string. */
[[nodiscard]] bool IsPrintableAscii(std::byte value) noexcept {
    const auto character = static_cast<unsigned char>(value);
    return character >= 0x20U && character <= 0x7EU;
}

}  // namespace

bool PeFile::Open(const std::filesystem::path& path, std::string& error) noexcept {
    try {
        path_.clear();
        bytes_.clear();
        sections_.clear();
        imageBase_ = 0;
        entryPointRva_ = 0;
        imageSize_ = 0;
        machine_ = 0;

        std::error_code sizeError;
        const std::uintmax_t fileSize = std::filesystem::file_size(path, sizeError);
        if (sizeError || fileSize < 0x100U || fileSize > kMaximumPeFileSize) {
            error = "PE file is missing, too small, or exceeds the defensive size limit";
            return false;
        }
        bytes_.resize(static_cast<std::size_t>(fileSize));
        std::ifstream stream(path, std::ios::binary);
        if (!stream) {
            error = "Could not open PE file";
            return false;
        }
        stream.read(reinterpret_cast<char*>(bytes_.data()), static_cast<std::streamsize>(bytes_.size()));
        if (stream.gcount() != static_cast<std::streamsize>(bytes_.size())) {
            error = "Could not read the complete PE file";
            return false;
        }
        if (bytes_[0] != std::byte{'M'} || bytes_[1] != std::byte{'Z'}) {
            error = "File has no DOS MZ signature";
            return false;
        }

        std::uint32_t ntOffset = 0;
        if (!ReadU32(bytes_, 0x3C, ntOffset) || ntOffset > bytes_.size() ||
            bytes_.size() - ntOffset < 24) {
            error = "DOS header points outside the PE file";
            return false;
        }
        if (std::memcmp(bytes_.data() + ntOffset, "PE\0\0", 4) != 0) {
            error = "File has no PE signature";
            return false;
        }

        std::uint16_t sectionCount = 0;
        std::uint16_t optionalHeaderSize = 0;
        std::uint16_t optionalMagic = 0;
        if (!ReadU16(bytes_, ntOffset + 4, machine_) ||
            !ReadU16(bytes_, ntOffset + 6, sectionCount) ||
            !ReadU16(bytes_, ntOffset + 20, optionalHeaderSize) ||
            !ReadU16(bytes_, ntOffset + 24, optionalMagic)) {
            error = "PE headers are truncated";
            return false;
        }
        if (machine_ != kI386Machine || optionalMagic != kPe32Magic || optionalHeaderSize < 96) {
            error = "Image is not a supported x86 PE32 file";
            return false;
        }
        if (!ReadU32(bytes_, ntOffset + 24 + 16, entryPointRva_) ||
            !ReadU32(bytes_, ntOffset + 24 + 28, imageBase_) ||
            !ReadU32(bytes_, ntOffset + 24 + 56, imageSize_)) {
            error = "PE optional header is truncated";
            return false;
        }

        const std::size_t sectionTable =
            static_cast<std::size_t>(ntOffset) + 24U + optionalHeaderSize;
        if (sectionCount == 0 || sectionCount > 96 || sectionTable > bytes_.size() ||
            static_cast<std::size_t>(sectionCount) * 40U > bytes_.size() - sectionTable) {
            error = "PE section table is invalid or truncated";
            return false;
        }
        sections_.reserve(sectionCount);
        for (std::uint16_t index = 0; index < sectionCount; ++index) {
            const std::size_t offset = sectionTable + static_cast<std::size_t>(index) * 40U;
            std::array<char, 9> name{};
            std::memcpy(name.data(), bytes_.data() + offset, 8);
            PeSection section{};
            section.name = name.data();
            if (!ReadU32(bytes_, offset + 8, section.virtualSize) ||
                !ReadU32(bytes_, offset + 12, section.virtualAddress) ||
                !ReadU32(bytes_, offset + 16, section.rawSize) ||
                !ReadU32(bytes_, offset + 20, section.rawOffset) ||
                !ReadU32(bytes_, offset + 36, section.characteristics)) {
                error = "PE section header is truncated";
                return false;
            }
            if (section.rawOffset > bytes_.size() || section.rawSize > bytes_.size() - section.rawOffset) {
                error = "PE section raw bytes point outside the file";
                return false;
            }
            sections_.push_back(std::move(section));
        }
        path_ = path;
        return true;
    } catch (...) {
        error = "PE parsing failed due to an allocation or filesystem exception";
        path_.clear();
        bytes_.clear();
        sections_.clear();
        return false;
    }
}

std::optional<std::uint32_t> PeFile::FileOffsetToRva(std::uint32_t fileOffset) const noexcept {
    for (const PeSection& section : sections_) {
        if (fileOffset >= section.rawOffset && fileOffset - section.rawOffset < section.rawSize) {
            return section.virtualAddress + (fileOffset - section.rawOffset);
        }
    }
    return std::nullopt;
}

std::vector<PeStringHit> PeFile::FindAsciiStrings(
    std::span<const std::string_view> needles,
    std::size_t minimumLength,
    std::size_t maximumHits) const {
    std::vector<PeStringHit> hits;
    std::vector<std::string> normalizedNeedles;
    normalizedNeedles.reserve(needles.size());
    for (const std::string_view needle : needles) {
        normalizedNeedles.push_back(LowerAscii(needle));
    }

    std::size_t offset = 0;
    while (offset < bytes_.size() && hits.size() < maximumHits) {
        if (!IsPrintableAscii(bytes_[offset])) {
            ++offset;
            continue;
        }
        const std::size_t start = offset;
        while (offset < bytes_.size() && IsPrintableAscii(bytes_[offset])) {
            ++offset;
        }
        if (offset - start < minimumLength) {
            continue;
        }
        const std::string text(
            reinterpret_cast<const char*>(bytes_.data() + start), offset - start);
        const std::string normalized = LowerAscii(text);
        bool matches = normalizedNeedles.empty();
        for (const std::string& needle : normalizedNeedles) {
            if (normalized.find(needle) != std::string::npos) {
                matches = true;
                break;
            }
        }
        if (matches && start <= (std::numeric_limits<std::uint32_t>::max)()) {
            hits.push_back(PeStringHit{
                text,
                static_cast<std::uint32_t>(start),
                FileOffsetToRva(static_cast<std::uint32_t>(start))});
        }
    }
    return hits;
}

std::vector<std::uint32_t> PeFile::FindValueInExecutableSections(
    std::uint32_t value, std::size_t maximumHits) const {
    std::vector<std::uint32_t> hits;
    const std::array<std::byte, 4> pattern{
        static_cast<std::byte>(value & 0xFFU),
        static_cast<std::byte>((value >> 8U) & 0xFFU),
        static_cast<std::byte>((value >> 16U) & 0xFFU),
        static_cast<std::byte>((value >> 24U) & 0xFFU)};
    for (const PeSection& section : sections_) {
        if (!section.isExecutable() || section.rawSize < pattern.size()) {
            continue;
        }
        const std::size_t start = section.rawOffset;
        const std::size_t end = start + section.rawSize - pattern.size() + 1U;
        for (std::size_t offset = start; offset < end && hits.size() < maximumHits; ++offset) {
            if (std::memcmp(bytes_.data() + offset, pattern.data(), pattern.size()) == 0) {
                hits.push_back(
                    section.virtualAddress + static_cast<std::uint32_t>(offset - section.rawOffset));
            }
        }
    }
    return hits;
}

std::vector<std::uint32_t> PeFile::FindValueInAllSections(
    std::uint32_t value, std::size_t maximumHits) const {
    std::vector<std::uint32_t> hits;
    const std::array<std::byte, 4> pattern{
        static_cast<std::byte>(value & 0xFFU),
        static_cast<std::byte>((value >> 8U) & 0xFFU),
        static_cast<std::byte>((value >> 16U) & 0xFFU),
        static_cast<std::byte>((value >> 24U) & 0xFFU)};
    for (const PeSection& section : sections_) {
        if (section.rawSize < pattern.size()) {
            continue;
        }
        const std::size_t start = section.rawOffset;
        const std::size_t end = start + section.rawSize - pattern.size() + 1U;
        for (std::size_t offset = start; offset < end && hits.size() < maximumHits; ++offset) {
            if (std::memcmp(bytes_.data() + offset, pattern.data(), pattern.size()) == 0) {
                hits.push_back(
                    section.virtualAddress + static_cast<std::uint32_t>(offset - section.rawOffset));
            }
        }
    }
    return hits;
}

std::vector<std::uint32_t> PeFile::FindRelativeCallReferences(
    std::uint32_t targetVa,
    std::size_t maximumHits) const {
    std::vector<std::uint32_t> hits;
    constexpr std::size_t instructionSize = 5U;
    for (const PeSection& section : sections_) {
        if (!section.isExecutable() || section.rawSize < instructionSize) {
            continue;
        }
        const std::size_t start = section.rawOffset;
        const std::size_t end = start + section.rawSize - instructionSize + 1U;
        for (std::size_t offset = start; offset < end && hits.size() < maximumHits; ++offset) {
            if (bytes_[offset] != std::byte{0xE8}) {
                continue;
            }
            std::int32_t displacement = 0;
            std::memcpy(&displacement, bytes_.data() + offset + 1U, sizeof(displacement));
            const std::uint32_t callRva =
                section.virtualAddress + static_cast<std::uint32_t>(offset - section.rawOffset);
            const std::int64_t nextVa =
                static_cast<std::int64_t>(imageBase_) + callRva + instructionSize;
            const std::int64_t destination = nextVa + displacement;
            if (destination == static_cast<std::int64_t>(targetVa)) {
                hits.push_back(callRva);
            }
        }
    }
    return hits;
}

const PeSection* PeFile::FindSectionForRva(std::uint32_t rva) const noexcept {
    for (const PeSection& section : sections_) {
        const std::uint32_t extent = (std::max)(section.virtualSize, section.rawSize);
        if (rva >= section.virtualAddress && rva - section.virtualAddress < extent) {
            return &section;
        }
    }
    return nullptr;
}

bool PeFile::WriteWithSectionCapture(
    std::string_view sectionName,
    const std::filesystem::path& capturePath,
    const std::filesystem::path& outputPath,
    std::string& error) const noexcept {
    try {
        const auto section = std::find_if(
            sections_.begin(), sections_.end(), [sectionName](const PeSection& candidate) {
                return candidate.name == sectionName;
            });
        if (section == sections_.end() || section->rawSize == 0 || section->virtualSize == 0) {
            error = "Requested PE section is absent or empty";
            return false;
        }

        std::error_code pathError;
        const std::filesystem::path inputCanonical = std::filesystem::weakly_canonical(path_, pathError);
        if (pathError) {
            error = "Could not canonicalize the source PE path";
            return false;
        }
        const std::filesystem::path outputCanonical =
            std::filesystem::weakly_canonical(outputPath, pathError);
        if (pathError || inputCanonical == outputCanonical) {
            error = "Output must be a distinct analysis file";
            return false;
        }
        if (std::filesystem::exists(outputPath, pathError) || pathError) {
            error = "Output already exists or cannot be inspected; refusing to overwrite it";
            return false;
        }

        const std::uintmax_t captureSize = std::filesystem::file_size(capturePath, pathError);
        if (pathError || captureSize != section->virtualSize) {
            error = "Runtime capture size does not exactly match the PE section virtual size";
            return false;
        }
        std::vector<std::byte> capture(static_cast<std::size_t>(captureSize));
        std::ifstream captureStream(capturePath, std::ios::binary);
        if (!captureStream) {
            error = "Could not open the runtime section capture";
            return false;
        }
        captureStream.read(
            reinterpret_cast<char*>(capture.data()), static_cast<std::streamsize>(capture.size()));
        if (captureStream.gcount() != static_cast<std::streamsize>(capture.size())) {
            error = "Could not read the complete runtime section capture";
            return false;
        }

        std::vector<std::byte> rebuilt = bytes_;
        const std::size_t copySize =
            (std::min)(static_cast<std::size_t>(section->rawSize), capture.size());
        std::memcpy(rebuilt.data() + section->rawOffset, capture.data(), copySize);

        const std::filesystem::path parent = outputPath.parent_path();
        if (!parent.empty()) {
            std::filesystem::create_directories(parent, pathError);
            if (pathError) {
                error = "Could not create the analysis output directory";
                return false;
            }
        }
        std::ofstream output(outputPath, std::ios::binary | std::ios::trunc);
        if (!output) {
            error = "Could not create the analysis PE copy";
            return false;
        }
        output.write(
            reinterpret_cast<const char*>(rebuilt.data()),
            static_cast<std::streamsize>(rebuilt.size()));
        if (!output) {
            output.close();
            std::error_code removeError;
            std::filesystem::remove(outputPath, removeError);
            error = "Could not write the complete analysis PE copy";
            return false;
        }
        return true;
    } catch (const std::exception& exception) {
        error = exception.what();
        return false;
    } catch (...) {
        error = "Unknown exception while rebuilding the analysis PE copy";
        return false;
    }
}

}  // namespace sc13::reverse
