#include "core/sha256.hpp"
#include "core/tgi.hpp"
#include "formats/dbpf/dbpf_reader.hpp"
#include "formats/prop/prop_document.hpp"

#include <Windows.h>

#include <algorithm>
#include <array>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <bit>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <optional>
#include <span>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace {

/** Owns one Windows handle used by the external memory scanner. */
class UniqueHandle final {
public:
    /** Takes ownership of a nullable Windows handle. */
    explicit UniqueHandle(HANDLE value = nullptr) noexcept : value_(value) {}

    /** Closes the owned handle. */
    ~UniqueHandle() {
        if (value_ != nullptr && value_ != INVALID_HANDLE_VALUE) {
            CloseHandle(value_);
        }
    }

    UniqueHandle(const UniqueHandle&) = delete;
    UniqueHandle& operator=(const UniqueHandle&) = delete;

    /** Returns the raw handle without transferring ownership. */
    [[nodiscard]] HANDLE get() const noexcept { return value_; }

    /** Reports whether the wrapper contains a usable handle. */
    [[nodiscard]] explicit operator bool() const noexcept {
        return value_ != nullptr && value_ != INVALID_HANDLE_VALUE;
    }

private:
    HANDLE value_ = nullptr;
};

/** Records one exact byte-pattern match in remote memory. */
struct MemoryHit final {
    std::uintptr_t address{};
    DWORD protection{};
    DWORD type{};
};

/** Converts ASCII-only command syntax from UTF-16. */
[[nodiscard]] std::optional<std::string> NarrowAscii(std::wstring_view text) {
    std::string result;
    result.reserve(text.size());
    for (const wchar_t character : text) {
        if (character < 0 || character > 0x7F) {
            return std::nullopt;
        }
        result.push_back(static_cast<char>(character));
    }
    return result;
}

/** Parses a decimal process identifier without accepting trailing characters. */
[[nodiscard]] std::optional<DWORD> ParseProcessId(std::wstring_view text) noexcept {
    const auto narrow = NarrowAscii(text);
    if (!narrow.has_value()) {
        return std::nullopt;
    }
    unsigned long value = 0;
    const auto parsed = std::from_chars(
        narrow->data(), narrow->data() + narrow->size(), value, 10);
    if (narrow->empty() || parsed.ec != std::errc{} ||
        parsed.ptr != narrow->data() + narrow->size() || value == 0 || value > MAXDWORD) {
        return std::nullopt;
    }
    return static_cast<DWORD>(value);
}

/** Parses a hexadecimal 32-bit identifier with an optional 0x prefix. */
[[nodiscard]] std::optional<std::uint32_t> ParseHex32(std::wstring_view text) noexcept {
    const auto narrow = NarrowAscii(text);
    if (!narrow.has_value()) {
        return std::nullopt;
    }
    std::string_view view = narrow.value();
    if (view.starts_with("0x") || view.starts_with("0X")) {
        view.remove_prefix(2);
    }
    std::uint32_t value = 0;
    const auto parsed = std::from_chars(view.data(), view.data() + view.size(), value, 16);
    if (view.empty() || parsed.ec != std::errc{} || parsed.ptr != view.data() + view.size()) {
        return std::nullopt;
    }
    return value;
}

/** Parses a hexadecimal address-sized integer with an optional 0x prefix. */
[[nodiscard]] std::optional<std::uintptr_t> ParseHexAddress(std::wstring_view text) noexcept {
    const auto narrow = NarrowAscii(text);
    if (!narrow.has_value()) {
        return std::nullopt;
    }
    std::string_view view = narrow.value();
    if (view.starts_with("0x") || view.starts_with("0X")) {
        view.remove_prefix(2);
    }
    std::uintptr_t value = 0;
    const auto parsed = std::from_chars(view.data(), view.data() + view.size(), value, 16);
    if (view.empty() || parsed.ec != std::errc{} || parsed.ptr != view.data() + view.size()) {
        return std::nullopt;
    }
    return value;
}

/** Writes a deterministic hexadecimal and ASCII view of one byte span. */
void AppendHexDump(
    std::ostringstream& report,
    std::uintptr_t baseAddress,
    std::span<const std::byte> bytes) {
    constexpr std::size_t bytesPerLine = 16;
    for (std::size_t offset = 0; offset < bytes.size(); offset += bytesPerLine) {
        report << "0x" << std::hex << std::uppercase << std::setw(8) << std::setfill('0')
               << baseAddress + offset << "  ";
        const std::size_t lineSize = (std::min)(bytesPerLine, bytes.size() - offset);
        for (std::size_t index = 0; index < bytesPerLine; ++index) {
            if (index < lineSize) {
                report << std::setw(2)
                       << std::to_integer<unsigned int>(bytes[offset + index]) << ' ';
            } else {
                report << "   ";
            }
        }
        report << " ";
        for (std::size_t index = 0; index < lineSize; ++index) {
            const unsigned int value = std::to_integer<unsigned int>(bytes[offset + index]);
            report << static_cast<char>(value >= 0x20U && value <= 0x7EU ? value : '.');
        }
        report << '\n';
    }
    report << std::dec;
}

/** Reads and reports a bounded neighborhood around one remote SimCity address. */
[[nodiscard]] int RunDump(int argumentCount, wchar_t** arguments) {
    if (argumentCount != 7) {
        return 2;
    }
    const auto processId = ParseProcessId(arguments[2]);
    const auto address = ParseHexAddress(arguments[3]);
    const auto before = ParseHexAddress(arguments[4]);
    const auto length = ParseHexAddress(arguments[5]);
    constexpr std::uintptr_t maximumDumpLength = 1024U * 1024U;
    if (!processId.has_value() || !address.has_value() || !before.has_value() ||
        !length.has_value() || length.value() == 0 || length.value() > maximumDumpLength ||
        before.value() > address.value()) {
        return 2;
    }

    UniqueHandle process(OpenProcess(
        PROCESS_QUERY_INFORMATION | PROCESS_QUERY_LIMITED_INFORMATION | PROCESS_VM_READ,
        FALSE, processId.value()));
    if (!process) {
        std::cerr << "OpenProcess failed with error " << GetLastError() << ".\n";
        return 1;
    }
    std::array<wchar_t, 32768> processPath{};
    DWORD pathLength = static_cast<DWORD>(processPath.size());
    if (QueryFullProcessImageNameW(
            process.get(), 0, processPath.data(), &pathLength) == FALSE ||
        _wcsicmp(std::filesystem::path(processPath.data(), processPath.data() + pathLength)
                     .filename()
                     .c_str(),
                 L"SimCity.exe") != 0) {
        std::cerr << "Target process is not SimCity.exe.\n";
        return 1;
    }

    const std::uintptr_t readAddress = address.value() - before.value();
    std::vector<std::byte> bytes(static_cast<std::size_t>(length.value()));
    SIZE_T bytesRead = 0;
    if (ReadProcessMemory(
            process.get(), reinterpret_cast<const void*>(readAddress), bytes.data(), bytes.size(),
            &bytesRead) == FALSE && bytesRead == 0) {
        std::cerr << "ReadProcessMemory failed with error " << GetLastError() << ".\n";
        return 1;
    }
    bytes.resize(bytesRead);

    std::ostringstream report;
    report << "pid=" << processId.value() << '\n'
           << "process=" << std::filesystem::path(
                  processPath.data(), processPath.data() + pathLength).string() << '\n'
           << "requested-address=0x" << std::hex << std::uppercase << address.value() << '\n'
           << "read-address=0x" << readAddress << '\n'
           << "requested-bytes=0x" << length.value() << '\n'
           << "bytes-read=0x" << bytesRead << '\n';
    AppendHexDump(report, readAddress, bytes);

    const std::string reportText = report.str();
    std::ofstream output(std::filesystem::path(arguments[6]), std::ios::binary | std::ios::trunc);
    if (!output) {
        std::cerr << "Could not create the report file.\n";
        return 1;
    }
    output.write(reportText.data(), static_cast<std::streamsize>(reportText.size()));
    if (!output) {
        std::cerr << "Could not write the complete report file.\n";
        return 1;
    }
    std::cout << reportText;
    return 0;
}

/** Returns true when a committed page protection permits ordinary reads. */
[[nodiscard]] bool IsReadableProtection(DWORD protection) noexcept {
    if ((protection & (PAGE_GUARD | PAGE_NOACCESS)) != 0) {
        return false;
    }
    const DWORD baseProtection = protection & 0xFFU;
    return baseProtection == PAGE_READONLY || baseProtection == PAGE_READWRITE ||
           baseProtection == PAGE_WRITECOPY || baseProtection == PAGE_EXECUTE_READ ||
           baseProtection == PAGE_EXECUTE_READWRITE || baseProtection == PAGE_EXECUTE_WRITECOPY;
}

/** Appends one 32-bit integer to a byte pattern in little-endian order. */
void AppendLittleU32(std::vector<std::byte>& bytes, std::uint32_t value) {
    bytes.push_back(static_cast<std::byte>(value & 0xFFU));
    bytes.push_back(static_cast<std::byte>((value >> 8U) & 0xFFU));
    bytes.push_back(static_cast<std::byte>((value >> 16U) & 0xFFU));
    bytes.push_back(static_cast<std::byte>((value >> 24U) & 0xFFU));
}

/** Appends one 32-bit integer to a byte pattern in big-endian order. */
void AppendBigU32(std::vector<std::byte>& bytes, std::uint32_t value) {
    bytes.push_back(static_cast<std::byte>((value >> 24U) & 0xFFU));
    bytes.push_back(static_cast<std::byte>((value >> 16U) & 0xFFU));
    bytes.push_back(static_cast<std::byte>((value >> 8U) & 0xFFU));
    bytes.push_back(static_cast<std::byte>(value & 0xFFU));
}

/** Finds exact occurrences of a pattern inside one locally copied remote-memory chunk. */
void FindInChunk(
    std::span<const std::byte> chunk,
    std::span<const std::byte> pattern,
    std::uintptr_t chunkAddress,
    DWORD protection,
    DWORD type,
    std::vector<MemoryHit>& hits,
    std::uintptr_t& lastAddress) {
    if (pattern.empty() || chunk.size() < pattern.size()) {
        return;
    }
    std::size_t offset = 0;
    while (offset <= chunk.size() - pattern.size() && hits.size() < 256) {
        const auto candidate = std::find(
            chunk.begin() + static_cast<std::ptrdiff_t>(offset), chunk.end(), pattern.front());
        if (candidate == chunk.end()) {
            break;
        }
        offset = static_cast<std::size_t>(candidate - chunk.begin());
        if (offset <= chunk.size() - pattern.size() &&
            std::memcmp(chunk.data() + offset, pattern.data(), pattern.size()) == 0) {
            const std::uintptr_t address = chunkAddress + offset;
            if (hits.empty() || address > lastAddress) {
                hits.push_back(MemoryHit{address, protection, type});
                lastAddress = address;
            }
        }
        ++offset;
    }
}

/** Scans every committed readable region of a process for one exact byte pattern. */
[[nodiscard]] bool ScanProcess(
    HANDLE process,
    std::span<const std::byte> pattern,
    std::vector<MemoryHit>& hits,
    std::string& error) {
    if (pattern.empty()) {
        error = "Memory pattern is empty";
        return false;
    }

    SYSTEM_INFO systemInfo{};
    GetSystemInfo(&systemInfo);
    std::uintptr_t cursor = reinterpret_cast<std::uintptr_t>(systemInfo.lpMinimumApplicationAddress);
    const std::uintptr_t maximum =
        reinterpret_cast<std::uintptr_t>(systemInfo.lpMaximumApplicationAddress);
    constexpr std::size_t chunkCapacity = 4U * 1024U * 1024U;
    std::vector<std::byte> buffer(chunkCapacity);
    std::uintptr_t lastAddress = 0;

    while (cursor < maximum && hits.size() < 256) {
        MEMORY_BASIC_INFORMATION information{};
        if (VirtualQueryEx(
                process, reinterpret_cast<const void*>(cursor), &information,
                sizeof(information)) != sizeof(information)) {
            cursor += 0x1000U;
            continue;
        }
        const std::uintptr_t regionBase =
            reinterpret_cast<std::uintptr_t>(information.BaseAddress);
        const std::uintptr_t nextRegion = regionBase + information.RegionSize;
        if (nextRegion <= cursor) {
            break;
        }
        cursor = nextRegion;
        if (information.State != MEM_COMMIT || !IsReadableProtection(information.Protect)) {
            continue;
        }

        const std::size_t regionSize = static_cast<std::size_t>(information.RegionSize);
        std::size_t regionOffset = 0;
        while (regionOffset < regionSize && hits.size() < 256) {
            const std::size_t requested = (std::min)(
                buffer.size(), regionSize - regionOffset);
            SIZE_T bytesRead = 0;
            const std::uintptr_t readAddress = regionBase + regionOffset;
            const BOOL readResult = ReadProcessMemory(
                process, reinterpret_cast<const void*>(readAddress), buffer.data(), requested,
                &bytesRead);
            if ((readResult != FALSE || bytesRead >= pattern.size()) && bytesRead != 0) {
                FindInChunk(
                    std::span<const std::byte>(buffer.data(), bytesRead), pattern, readAddress,
                    information.Protect, information.Type, hits, lastAddress);
            }
            if (requested == regionSize - regionOffset) {
                break;
            }
            const std::size_t overlap = (std::min)(pattern.size() - 1U, requested - 1U);
            regionOffset += requested - overlap;
        }
    }
    return true;
}

/** Formats one memory scan and all of its exact remote addresses. */
void AppendScanReport(
    std::ostringstream& report,
    std::string_view label,
    std::span<const std::byte> pattern,
    HANDLE process) {
    std::vector<MemoryHit> hits;
    std::string error;
    const bool succeeded = ScanProcess(process, pattern, hits, error);
    report << "pattern=" << label << " bytes=" << pattern.size();
    if (!succeeded) {
        report << " error=" << error << '\n';
        return;
    }
    report << " hits=" << hits.size() << '\n';
    for (const MemoryHit& hit : hits) {
        report << "  address=0x" << std::hex << std::uppercase << std::setw(8)
               << std::setfill('0') << hit.address << " protection=0x" << std::setw(8)
               << hit.protection << " type=0x" << std::setw(8) << hit.type << std::dec << '\n';
    }
}

/** Prints deterministic command syntax for the external resource-memory probe. */
void PrintUsage() {
    std::wcerr
        << L"Usage:\n"
        << L"  sc13-memory-probe resource <pid> <package> <TGI> <property-id> <report-file>\n"
        << L"  sc13-memory-probe dump <pid> <address-hex> <bytes-before-hex> <length-hex> "
           L"<report-file>\n";
}

}  // namespace

/** Locates exact resource, TGI, and property encodings in a live SimCity process. */
int wmain(int argumentCount, wchar_t** arguments) {
    if (argumentCount == 7 && std::wstring_view(arguments[1]) == L"dump") {
        const int result = RunDump(argumentCount, arguments);
        if (result == 2) {
            PrintUsage();
        }
        return result;
    }
    if (argumentCount != 7 || std::wstring_view(arguments[1]) != L"resource") {
        PrintUsage();
        return 2;
    }
    const auto processId = ParseProcessId(arguments[2]);
    const auto tgiText = NarrowAscii(arguments[4]);
    const auto tgi = tgiText.has_value() ? sc13::core::ParseTgi(tgiText.value()) : std::nullopt;
    const auto propertyId = ParseHex32(arguments[5]);
    if (!processId.has_value() || !tgi.has_value() || !propertyId.has_value()) {
        PrintUsage();
        return 2;
    }

    sc13::formats::dbpf::DbpfPackage package;
    std::string error;
    if (!package.Open(std::filesystem::path(arguments[3]), error)) {
        std::cerr << "Package open failed: " << error << '\n';
        return 1;
    }
    const sc13::formats::dbpf::DbpfEntry* const entry = package.Find(tgi.value());
    if (entry == nullptr) {
        std::cerr << "Exact TGI was not found in the package.\n";
        return 1;
    }
    std::vector<std::byte> resource;
    if (!package.ReadResource(*entry, resource, error)) {
        std::cerr << "Resource read failed: " << error << '\n';
        return 1;
    }

    UniqueHandle process(OpenProcess(
        PROCESS_QUERY_INFORMATION | PROCESS_QUERY_LIMITED_INFORMATION | PROCESS_VM_READ,
        FALSE, processId.value()));
    if (!process) {
        std::cerr << "OpenProcess failed with error " << GetLastError() << ".\n";
        return 1;
    }
    std::array<wchar_t, 32768> processPath{};
    DWORD pathLength = static_cast<DWORD>(processPath.size());
    if (QueryFullProcessImageNameW(
            process.get(), 0, processPath.data(), &pathLength) == FALSE ||
        _wcsicmp(std::filesystem::path(processPath.data(), processPath.data() + pathLength)
                     .filename()
                     .c_str(),
                 L"SimCity.exe") != 0) {
        std::cerr << "Target process is not SimCity.exe.\n";
        return 1;
    }

    sc13::core::Sha256Digest digest{};
    if (!sc13::core::Sha256(resource, digest, error)) {
        std::cerr << "Resource digest failed: " << error << '\n';
        return 1;
    }

    std::vector<std::byte> tgiLittle12;
    AppendLittleU32(tgiLittle12, tgi->type);
    AppendLittleU32(tgiLittle12, tgi->group);
    AppendLittleU32(tgiLittle12, tgi->instance);
    std::vector<std::byte> tgiLittle16LowHigh = tgiLittle12;
    AppendLittleU32(tgiLittle16LowHigh, 0);
    std::vector<std::byte> tgiLittle16HighLow;
    AppendLittleU32(tgiLittle16HighLow, tgi->type);
    AppendLittleU32(tgiLittle16HighLow, tgi->group);
    AppendLittleU32(tgiLittle16HighLow, 0);
    AppendLittleU32(tgiLittle16HighLow, tgi->instance);
    std::vector<std::byte> tgiBig12;
    AppendBigU32(tgiBig12, tgi->type);
    AppendBigU32(tgiBig12, tgi->group);
    AppendBigU32(tgiBig12, tgi->instance);

    std::array<std::uint32_t, 3> orderedTgi{
        tgi->type, tgi->group, tgi->instance};
    std::vector<std::pair<std::string, std::vector<std::byte>>> tgiPermutations;
    std::sort(orderedTgi.begin(), orderedTgi.end());
    do {
        std::vector<std::byte> pattern;
        for (const std::uint32_t word : orderedTgi) {
            AppendLittleU32(pattern, word);
        }
        std::ostringstream label;
        label << "tgi-le12-order-" << std::hex << std::uppercase
              << orderedTgi[0] << '-' << orderedTgi[1] << '-' << orderedTgi[2];
        tgiPermutations.emplace_back(label.str(), std::move(pattern));
    } while (std::next_permutation(orderedTgi.begin(), orderedTgi.end()));

    std::vector<std::byte> typeLittle;
    AppendLittleU32(typeLittle, tgi->type);
    std::vector<std::byte> groupLittle;
    AppendLittleU32(groupLittle, tgi->group);
    std::vector<std::byte> instanceLittle;
    AppendLittleU32(instanceLittle, tgi->instance);
    std::vector<std::byte> propertyIdLittle;
    AppendLittleU32(propertyIdLittle, propertyId.value());
    std::vector<std::byte> propertyIdBig;
    AppendBigU32(propertyIdBig, propertyId.value());
    constexpr float expectedFloat = 300.0F;
    const std::uint32_t floatBits = std::bit_cast<std::uint32_t>(expectedFloat);
    constexpr float replacementFloat = 345.0F;
    const std::uint32_t replacementFloatBits =
        std::bit_cast<std::uint32_t>(replacementFloat);
    std::vector<std::byte> floatLittle;
    AppendLittleU32(floatLittle, floatBits);
    std::vector<std::byte> floatBig;
    AppendBigU32(floatBig, floatBits);
    std::vector<std::byte> runtimeProperty300 = propertyIdLittle;
    AppendLittleU32(runtimeProperty300, floatBits);
    std::vector<std::byte> runtimeProperty345 = propertyIdLittle;
    AppendLittleU32(runtimeProperty345, replacementFloatBits);

    sc13::formats::prop::PropDocument document;
    if (!document.Parse(resource, error)) {
        std::cerr << "PROP parse failed: " << error << '\n';
        return 1;
    }
    const sc13::formats::prop::PropertyRecord* const property = document.Find(propertyId.value());
    if (property == nullptr || property->recordOffset > resource.size() ||
        property->recordSize > resource.size() - property->recordOffset) {
        std::cerr << "Requested property record is absent or invalid.\n";
        return 1;
    }
    const std::span<const std::byte> propertyRecord(
        resource.data() + property->recordOffset, property->recordSize);

    std::ostringstream report;
    report << "pid=" << processId.value() << '\n'
           << "process=" << std::filesystem::path(
                  processPath.data(), processPath.data() + pathLength).string() << '\n'
           << "package=" << package.path().string() << '\n'
           << "tgi=" << sc13::core::ToString(tgi.value()) << '\n'
           << "resource-bytes=" << resource.size() << '\n'
           << "resource-sha256=" << sc13::core::ToHex(digest) << '\n'
           << "property-id=0x" << std::hex << std::uppercase << std::setw(8)
           << std::setfill('0') << propertyId.value() << std::dec << '\n';
    if (const auto value = document.GetFloat(propertyId.value()); value.has_value()) {
        report << "property-float=" << value.value() << '\n';
    }
    AppendScanReport(report, "full-resource", resource, process.get());
    AppendScanReport(report, "tgi-le12", tgiLittle12, process.get());
    AppendScanReport(report, "tgi-le16-low-high", tgiLittle16LowHigh, process.get());
    AppendScanReport(report, "tgi-le16-high-low", tgiLittle16HighLow, process.get());
    AppendScanReport(report, "tgi-be12", tgiBig12, process.get());
    for (const auto& [label, pattern] : tgiPermutations) {
        AppendScanReport(report, label, pattern, process.get());
    }
    AppendScanReport(report, "type-le32", typeLittle, process.get());
    AppendScanReport(report, "group-le32", groupLittle, process.get());
    AppendScanReport(report, "instance-le32", instanceLittle, process.get());
    AppendScanReport(report, "property-id-le32", propertyIdLittle, process.get());
    AppendScanReport(report, "property-id-be32", propertyIdBig, process.get());
    AppendScanReport(report, "float-300-le32", floatLittle, process.get());
    AppendScanReport(report, "float-300-be32", floatBig, process.get());
    AppendScanReport(report, "runtime-property-id-float-300-le8", runtimeProperty300, process.get());
    AppendScanReport(report, "runtime-property-id-float-345-le8", runtimeProperty345, process.get());
    AppendScanReport(report, "property-record", propertyRecord, process.get());

    const std::string reportText = report.str();
    std::ofstream output(std::filesystem::path(arguments[6]), std::ios::binary | std::ios::trunc);
    if (!output) {
        std::cerr << "Could not create the report file.\n";
        return 1;
    }
    output.write(reportText.data(), static_cast<std::streamsize>(reportText.size()));
    if (!output) {
        std::cerr << "Could not write the complete report file.\n";
        return 1;
    }
    std::cout << reportText;
    return 0;
}
