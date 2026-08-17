#include "core/sha256.hpp"
#include "core/tgi.hpp"
#include "formats/dbpf/dbpf_reader.hpp"
#include "formats/prop/prop_document.hpp"

#include <charconv>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace {

using sc13::formats::dbpf::DbpfEntry;
using sc13::formats::dbpf::DbpfPackage;

/** Converts command-line ASCII syntax to a narrow string without locale-dependent behavior. */
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

/** Parses one hexadecimal 32-bit identifier with an optional 0x prefix. */
[[nodiscard]] std::optional<std::uint32_t> ParseIdentifier(std::string_view text) noexcept {
    if (text.starts_with("0x") || text.starts_with("0X")) {
        text.remove_prefix(2);
    }
    std::uint32_t value = 0;
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value, 16);
    if (text.empty() || parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size()) {
        return std::nullopt;
    }
    return value;
}

/** Prints a concise command-line usage description. */
void PrintUsage() {
    std::wcerr
        << L"Usage:\n"
        << L"  sc13-inspect file <package> <TYPE:GROUP:INSTANCE> [property-id]\n"
        << L"  sc13-inspect scan <directory> <TYPE:GROUP:INSTANCE> [property-id]\n";
}

/** Inspects one exact resource match and reports its lossless PROP parse status. */
[[nodiscard]] bool InspectMatch(
    const DbpfPackage& package,
    const DbpfEntry& entry,
    const std::optional<std::uint32_t>& propertyIdentifier) {
    std::vector<std::byte> resource;
    std::string error;
    if (!package.ReadResource(entry, resource, error)) {
        std::wcerr << L"  extraction error: " << error.c_str() << L"\n";
        return false;
    }

    sc13::core::Sha256Digest digest{};
    if (!sc13::core::Sha256(resource, digest, error)) {
        std::wcerr << L"  SHA-256 error: " << error.c_str() << L"\n";
        return false;
    }

    std::wcout << L"MATCH " << package.path().wstring() << L"\n"
               << L"  TGI          " << sc13::core::ToString(entry.tgi).c_str() << L"\n"
               << L"  offset       0x" << std::hex << std::uppercase << std::setw(8)
               << std::setfill(L'0') << entry.chunkOffset << L"\n"
               << L"  disk/memory  " << std::dec << entry.diskSize << L" / " << entry.memorySize
               << L" bytes\n"
               << L"  compression  0x" << std::hex << std::setw(4) << entry.compression << L"\n"
               << L"  resource SHA " << sc13::core::ToHex(digest).c_str() << L"\n";

    sc13::formats::prop::PropDocument document;
    error.clear();
    if (!document.Parse(resource, error)) {
        std::wcout << L"  PROP parse   FAILED: " << error.c_str() << L"\n";
        return false;
    }
    std::wcout << L"  PROP parse   OK, " << std::dec << document.properties().size()
               << L" properties, lossless byte buffer\n";

    if (propertyIdentifier.has_value()) {
        const auto value = document.GetFloat(propertyIdentifier.value());
        if (!value.has_value()) {
            std::wcout << L"  property     0x" << std::hex << std::setw(8)
                       << propertyIdentifier.value() << L" is absent or not a scalar float\n";
            return false;
        }
        std::wcout << L"  property     0x" << std::hex << std::setw(8)
                   << propertyIdentifier.value() << std::dec << L" = " << value.value() << L"\n";
    }
    return true;
}

/** Opens one package and inspects the requested TGI when present. */
[[nodiscard]] bool InspectPackage(
    const std::filesystem::path& path,
    const sc13::core::Tgi& tgi,
    const std::optional<std::uint32_t>& propertyIdentifier,
    bool reportOpenErrors,
    bool& found) {
    DbpfPackage package;
    std::string error;
    if (!package.Open(path, error)) {
        if (reportOpenErrors) {
            std::wcerr << L"Could not parse " << path.wstring() << L": " << error.c_str() << L"\n";
        }
        return !reportOpenErrors;
    }
    const DbpfEntry* entry = package.Find(tgi);
    if (entry == nullptr) {
        return true;
    }
    found = true;
    return InspectMatch(package, *entry, propertyIdentifier);
}

}  // namespace

/** Runs the read-only DBPF/TGI inspection utility. */
int wmain(int argumentCount, wchar_t** arguments) {
    if (argumentCount < 4 || argumentCount > 5) {
        PrintUsage();
        return 2;
    }

    const auto mode = NarrowAscii(arguments[1]);
    const auto tgiText = NarrowAscii(arguments[3]);
    if (!mode.has_value() || !tgiText.has_value()) {
        std::wcerr << L"Mode and identifiers must use ASCII syntax.\n";
        return 2;
    }
    const auto tgi = sc13::core::ParseTgi(tgiText.value());
    if (!tgi.has_value()) {
        std::wcerr << L"Invalid TGI syntax.\n";
        return 2;
    }

    std::optional<std::uint32_t> propertyIdentifier;
    if (argumentCount == 5) {
        const auto propertyText = NarrowAscii(arguments[4]);
        if (!propertyText.has_value()) {
            std::wcerr << L"Property identifier must use ASCII hexadecimal syntax.\n";
            return 2;
        }
        propertyIdentifier = ParseIdentifier(propertyText.value());
        if (!propertyIdentifier.has_value()) {
            std::wcerr << L"Invalid property identifier.\n";
            return 2;
        }
    }

    const std::filesystem::path input = arguments[2];
    bool found = false;
    bool success = true;
    if (mode.value() == "file") {
        success = InspectPackage(input, tgi.value(), propertyIdentifier, true, found);
    } else if (mode.value() == "scan") {
        std::error_code iteratorError;
        const std::filesystem::recursive_directory_iterator end;
        for (std::filesystem::recursive_directory_iterator iterator(
                 input, std::filesystem::directory_options::skip_permission_denied, iteratorError);
             iterator != end;
             iterator.increment(iteratorError)) {
            if (iteratorError) {
                iteratorError.clear();
                continue;
            }
            if (iterator->is_regular_file() && iterator->path().extension() == L".package") {
                success = InspectPackage(
                              iterator->path(), tgi.value(), propertyIdentifier, false, found) && success;
            }
        }
    } else {
        PrintUsage();
        return 2;
    }

    if (!found) {
        std::wcout << L"No exact TGI match found.\n";
        return 3;
    }
    return success ? 0 : 1;
}
