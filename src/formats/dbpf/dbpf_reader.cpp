#include "formats/dbpf/dbpf_reader.hpp"

#include "formats/dbpf/refpack.hpp"

#include <array>
#include <cstring>
#include <fstream>
#include <limits>

namespace sc13::formats::dbpf {
namespace {

constexpr std::size_t kHeaderSize = 96;
constexpr std::uint32_t kMaximumEntryCount = 10'000'000U;

/** Reads little-endian integers from a bounded byte sequence. */
class LittleEndianReader final {
public:
    explicit LittleEndianReader(std::span<const std::byte> data) : data_(data) {}

    /** Reads one little-endian 32-bit value. */
    [[nodiscard]] bool ReadU32(std::uint32_t& value) noexcept {
        if (data_.size() - offset_ < sizeof(value)) {
            return false;
        }
        const auto* bytes = data_.data() + offset_;
        value = static_cast<std::uint32_t>(bytes[0]) |
                (static_cast<std::uint32_t>(bytes[1]) << 8U) |
                (static_cast<std::uint32_t>(bytes[2]) << 16U) |
                (static_cast<std::uint32_t>(bytes[3]) << 24U);
        offset_ += sizeof(value);
        return true;
    }

    /** Reads one little-endian 16-bit value. */
    [[nodiscard]] bool ReadU16(std::uint16_t& value) noexcept {
        if (data_.size() - offset_ < sizeof(value)) {
            return false;
        }
        const auto* bytes = data_.data() + offset_;
        value = static_cast<std::uint16_t>(
            static_cast<std::uint16_t>(bytes[0]) |
            (static_cast<std::uint16_t>(bytes[1]) << 8U));
        offset_ += sizeof(value);
        return true;
    }

    /** Returns the number of bytes consumed. */
    [[nodiscard]] std::size_t offset() const noexcept { return offset_; }

private:
    std::span<const std::byte> data_;
    std::size_t offset_{};
};

/** Reads a little-endian 32-bit value at a fixed header offset. */
[[nodiscard]] std::uint32_t ReadHeaderU32(
    const std::array<std::byte, kHeaderSize>& header, std::size_t offset) noexcept {
    return static_cast<std::uint32_t>(header[offset]) |
           (static_cast<std::uint32_t>(header[offset + 1]) << 8U) |
           (static_cast<std::uint32_t>(header[offset + 2]) << 16U) |
           (static_cast<std::uint32_t>(header[offset + 3]) << 24U);
}

/** Checks whether an offset plus size stays inside a file without overflowing. */
[[nodiscard]] bool IsRangeInside(
    std::uint64_t offset, std::uint64_t size, std::uint64_t containerSize) noexcept {
    return offset <= containerSize && size <= containerSize - offset;
}

}  // namespace

bool DbpfPackage::Open(const std::filesystem::path& path, std::string& error) noexcept {
    try {
        path_.clear();
        entries_.clear();
        fileSize_ = 0;
        majorVersion_ = 0;
        minorVersion_ = 0;
        indexFlags_ = 0;

        std::error_code sizeError;
        const std::uintmax_t measuredSize = std::filesystem::file_size(path, sizeError);
        if (sizeError || measuredSize < kHeaderSize ||
            measuredSize > (std::numeric_limits<std::uint64_t>::max)()) {
            error = "Package is missing, too short, or its size cannot be read";
            return false;
        }
        fileSize_ = static_cast<std::uint64_t>(measuredSize);

        std::ifstream stream(path, std::ios::binary);
        if (!stream) {
            error = "Could not open package";
            return false;
        }

        std::array<std::byte, kHeaderSize> header{};
        stream.read(reinterpret_cast<char*>(header.data()), static_cast<std::streamsize>(header.size()));
        if (stream.gcount() != static_cast<std::streamsize>(header.size()) ||
            std::memcmp(header.data(), "DBPF", 4) != 0) {
            error = "File does not contain a complete DBPF header";
            return false;
        }

        majorVersion_ = ReadHeaderU32(header, 0x04);
        minorVersion_ = ReadHeaderU32(header, 0x08);
        const std::uint32_t entryCount = ReadHeaderU32(header, 0x24);
        const std::uint32_t indexSize = ReadHeaderU32(header, 0x2C);
        const std::uint32_t indexMinorVersion = ReadHeaderU32(header, 0x3C);
        const std::uint32_t indexOffset = ReadHeaderU32(header, 0x40);
        if (majorVersion_ != 3U || indexMinorVersion != 3U) {
            error = "Package is not the SimCity DBPF v3/index-v3 layout";
            return false;
        }
        if (entryCount > kMaximumEntryCount) {
            error = "DBPF entry count exceeds the defensive limit";
            return false;
        }
        if (!IsRangeInside(indexOffset, indexSize, fileSize_)) {
            error = "DBPF index lies outside the file";
            return false;
        }

        std::vector<std::byte> index(indexSize);
        stream.seekg(static_cast<std::streamoff>(indexOffset), std::ios::beg);
        stream.read(reinterpret_cast<char*>(index.data()), static_cast<std::streamsize>(index.size()));
        if (stream.gcount() != static_cast<std::streamsize>(index.size())) {
            error = "DBPF index could not be read completely";
            return false;
        }

        LittleEndianReader reader(index);
        if (!reader.ReadU32(indexFlags_) || (indexFlags_ & ~0x7U) != 0) {
            error = "DBPF index uses unsupported constant-field flags";
            return false;
        }

        std::optional<std::uint32_t> fixedType;
        std::optional<std::uint32_t> fixedGroup;
        std::optional<std::uint32_t> fixedInstanceHigh;
        std::uint32_t fixedValue = 0;
        if ((indexFlags_ & 0x1U) != 0) {
            if (!reader.ReadU32(fixedValue)) {
                error = "DBPF index is truncated in its fixed type";
                return false;
            }
            fixedType = fixedValue;
        }
        if ((indexFlags_ & 0x2U) != 0) {
            if (!reader.ReadU32(fixedValue)) {
                error = "DBPF index is truncated in its fixed group";
                return false;
            }
            fixedGroup = fixedValue;
        }
        if ((indexFlags_ & 0x4U) != 0) {
            if (!reader.ReadU32(fixedValue)) {
                error = "DBPF index is truncated in its fixed instance-high field";
                return false;
            }
            fixedInstanceHigh = fixedValue;
        }

        entries_.reserve(entryCount);
        for (std::uint32_t indexNumber = 0; indexNumber < entryCount; ++indexNumber) {
            DbpfEntry entry{};
            std::uint32_t diskSizeRaw = 0;
            if (fixedType.has_value()) {
                entry.tgi.type = fixedType.value();
            } else if (!reader.ReadU32(entry.tgi.type)) {
                error = "DBPF index is truncated while reading an entry type";
                return false;
            }
            if (fixedGroup.has_value()) {
                entry.tgi.group = fixedGroup.value();
            } else if (!reader.ReadU32(entry.tgi.group)) {
                error = "DBPF index is truncated while reading an entry group";
                return false;
            }
            if (fixedInstanceHigh.has_value()) {
                entry.instanceHigh = fixedInstanceHigh.value();
            } else if (!reader.ReadU32(entry.instanceHigh)) {
                error = "DBPF index is truncated while reading instance-high";
                return false;
            }
            if (!reader.ReadU32(entry.tgi.instance) || !reader.ReadU32(entry.chunkOffset) ||
                !reader.ReadU32(diskSizeRaw) || !reader.ReadU32(entry.memorySize) ||
                !reader.ReadU16(entry.compression) || !reader.ReadU16(entry.unknown)) {
                error = "DBPF index is truncated in an entry body";
                return false;
            }

            entry.diskSizeHighBit = (diskSizeRaw & 0x80000000U) != 0;
            entry.diskSize = diskSizeRaw & 0x7FFFFFFFU;
            if (!IsRangeInside(entry.chunkOffset, entry.diskSize, fileSize_)) {
                error = "DBPF resource points outside the package";
                return false;
            }
            entries_.push_back(entry);
        }

        if (reader.offset() != index.size()) {
            error = "DBPF index size does not match the parsed entries";
            return false;
        }

        path_ = path;
        return true;
    } catch (...) {
        error = "DBPF parsing failed due to an allocation or filesystem exception";
        path_.clear();
        entries_.clear();
        return false;
    }
}

const DbpfEntry* DbpfPackage::Find(const core::Tgi& tgi) const noexcept {
    for (const DbpfEntry& entry : entries_) {
        if (entry.tgi == tgi) {
            return &entry;
        }
    }
    return nullptr;
}

bool DbpfPackage::ReadResource(
    const DbpfEntry& entry, std::vector<std::byte>& data, std::string& error) const noexcept {
    try {
        data.clear();
        if (path_.empty() || !IsRangeInside(entry.chunkOffset, entry.diskSize, fileSize_)) {
            error = "Resource does not belong to an open package or is out of bounds";
            return false;
        }

        std::ifstream stream(path_, std::ios::binary);
        if (!stream) {
            error = "Could not reopen package to read resource";
            return false;
        }
        std::vector<std::byte> stored(entry.diskSize);
        stream.seekg(static_cast<std::streamoff>(entry.chunkOffset), std::ios::beg);
        stream.read(reinterpret_cast<char*>(stored.data()), static_cast<std::streamsize>(stored.size()));
        if (stream.gcount() != static_cast<std::streamsize>(stored.size())) {
            error = "Resource bytes could not be read completely";
            return false;
        }

        if (entry.compression == 0xFFFFU) {
            return DecompressRefPack(stored, entry.memorySize, data, error);
        }
        if (entry.compression != 0x0000U) {
            error = "Resource uses an unsupported DBPF compression marker";
            return false;
        }
        if (entry.memorySize != entry.diskSize) {
            error = "Uncompressed resource has inconsistent disk and memory sizes";
            return false;
        }
        data = std::move(stored);
        return true;
    } catch (...) {
        error = "Resource extraction failed due to an allocation or I/O exception";
        data.clear();
        return false;
    }
}

}  // namespace sc13::formats::dbpf
