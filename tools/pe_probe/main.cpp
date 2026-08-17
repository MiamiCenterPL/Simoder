#include "reverse/pe_file.hpp"

#include <array>
#include <charconv>
#include <cstdint>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace {

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

/** Parses one hexadecimal 32-bit value with an optional 0x prefix. */
[[nodiscard]] std::optional<std::uint32_t> ParseHex32(std::string_view text) noexcept {
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

/** Prints deterministic usage for offline PE analysis modes. */
void PrintUsage() {
    std::cerr
        << "Usage:\n"
        << "  sc13-pe-probe summary <pe-file>\n"
        << "  sc13-pe-probe strings <pe-file> [ASCII-needle]\n"
        << "  sc13-pe-probe xref-string <pe-file> <ASCII-needle>\n"
        << "  sc13-pe-probe value <pe-file> <hex32>\n"
        << "  sc13-pe-probe xref-call <pe-file> <target-va-hex>\n"
        << "  sc13-pe-probe rebuild-text <pe-file> <text-capture> <analysis-output>\n";
}

/** Prints one string location using raw, RVA, and preferred VA coordinates. */
void PrintStringHit(const sc13::reverse::PeFile& pe, const sc13::reverse::PeStringHit& hit) {
    std::cout << "file=0x" << std::hex << std::uppercase << std::setw(8) << std::setfill('0')
              << hit.fileOffset;
    if (hit.rva.has_value()) {
        std::cout << " rva=0x" << std::setw(8) << hit.rva.value()
                  << " va=0x" << std::setw(8) << pe.imageBase() + hit.rva.value();
    } else {
        std::cout << " rva=unmapped";
    }
    std::cout << " text=" << hit.text << '\n';
}

}  // namespace

/** Runs read-only PE summary, string, xref, and immediate-value analysis. */
int wmain(int argumentCount, wchar_t** arguments) {
    if (argumentCount < 3 || argumentCount > 5) {
        PrintUsage();
        return 2;
    }
    const auto mode = NarrowAscii(arguments[1]);
    if (!mode.has_value()) {
        PrintUsage();
        return 2;
    }

    sc13::reverse::PeFile pe;
    std::string error;
    if (!pe.Open(std::filesystem::path(arguments[2]), error)) {
        std::cerr << "PE open failed: " << error << '\n';
        return 1;
    }

    if (mode.value() == "rebuild-text") {
        if (argumentCount != 5) {
            PrintUsage();
            return 2;
        }
        if (!pe.WriteWithSectionCapture(
                ".text", std::filesystem::path(arguments[3]),
                std::filesystem::path(arguments[4]), error)) {
            std::cerr << "Analysis PE rebuild failed: " << error << '\n';
            return 1;
        }
        std::wcout << L"Analysis-only PE copy written: " << arguments[4] << L'\n';
        return 0;
    }

    if (mode.value() == "summary") {
        std::cout << "machine=0x" << std::hex << std::uppercase << std::setw(4)
                  << std::setfill('0') << pe.machine()
                  << " imageBase=0x" << std::setw(8) << pe.imageBase()
                  << " entryRva=0x" << std::setw(8) << pe.entryPointRva()
                  << " imageSize=0x" << std::setw(8) << pe.imageSize() << '\n';
        for (const sc13::reverse::PeSection& section : pe.sections()) {
            std::cout << "section=" << section.name
                      << " rva=0x" << std::setw(8) << section.virtualAddress
                      << " virtual=0x" << std::setw(8) << section.virtualSize
                      << " rawOffset=0x" << std::setw(8) << section.rawOffset
                      << " rawSize=0x" << std::setw(8) << section.rawSize
                      << " executable=" << (section.isExecutable() ? "yes" : "no") << '\n';
        }
        return 0;
    }

    if (mode.value() == "strings") {
        std::vector<std::string_view> needles;
        std::string customNeedle;
        if (argumentCount == 4) {
            const auto converted = NarrowAscii(arguments[3]);
            if (!converted.has_value()) {
                std::cerr << "Needle must be ASCII.\n";
                return 2;
            }
            customNeedle = converted.value();
            needles.push_back(customNeedle);
        } else {
            needles = {
                "package", "dbpf", "refpack", "resource", "property", "spark",
                "createfile", "readfile", "filemapping", "mapviewoffile"};
        }
        const auto hits = pe.FindAsciiStrings(needles, 4, 512);
        for (const auto& hit : hits) {
            PrintStringHit(pe, hit);
        }
        std::cout << "hits=" << std::dec << hits.size() << '\n';
        return 0;
    }

    if (mode.value() == "xref-string") {
        if (argumentCount != 4) {
            PrintUsage();
            return 2;
        }
        const auto converted = NarrowAscii(arguments[3]);
        if (!converted.has_value()) {
            std::cerr << "Needle must be ASCII.\n";
            return 2;
        }
        const std::string needle = converted.value();
        const std::array<std::string_view, 1> needles{needle};
        const auto hits = pe.FindAsciiStrings(needles, 4, 64);
        for (const auto& hit : hits) {
            PrintStringHit(pe, hit);
            if (!hit.rva.has_value()) {
                continue;
            }
            const std::uint32_t va = pe.imageBase() + hit.rva.value();
            const auto references = pe.FindValueInAllSections(va, 256);
            for (const std::uint32_t reference : references) {
                const sc13::reverse::PeSection* section = pe.FindSectionForRva(reference);
                std::cout << "  xref-rva=0x" << std::hex << std::uppercase << std::setw(8)
                          << std::setfill('0') << reference
                          << " xref-va=0x" << std::setw(8) << pe.imageBase() + reference
                          << " section=" << (section != nullptr ? section->name : "unknown") << '\n';
                if (section != nullptr && !section->isExecutable()) {
                    const std::uint32_t referenceVa = pe.imageBase() + reference;
                    const auto codeReferences = pe.FindValueInExecutableSections(referenceVa, 256);
                    for (const std::uint32_t codeReference : codeReferences) {
                        std::cout << "    code-xref-rva=0x" << std::setw(8) << codeReference
                                  << " code-xref-va=0x" << std::setw(8)
                                  << pe.imageBase() + codeReference << '\n';
                    }
                }
            }
        }
        std::cout << "strings=" << std::dec << hits.size() << '\n';
        return 0;
    }

    if (mode.value() == "value") {
        if (argumentCount != 4) {
            PrintUsage();
            return 2;
        }
        const auto converted = NarrowAscii(arguments[3]);
        const auto value = converted.has_value() ? ParseHex32(converted.value()) : std::nullopt;
        if (!value.has_value()) {
            std::cerr << "Value must be a hexadecimal uint32.\n";
            return 2;
        }
        const auto hits = pe.FindValueInExecutableSections(value.value());
        for (const std::uint32_t rva : hits) {
            std::cout << "rva=0x" << std::hex << std::uppercase << std::setw(8)
                      << std::setfill('0') << rva
                      << " va=0x" << std::setw(8) << pe.imageBase() + rva << '\n';
        }
        std::cout << "hits=" << std::dec << hits.size() << '\n';
        return 0;
    }

    if (mode.value() == "xref-call") {
        if (argumentCount != 4) {
            PrintUsage();
            return 2;
        }
        const auto converted = NarrowAscii(arguments[3]);
        const auto target = converted.has_value() ? ParseHex32(converted.value()) : std::nullopt;
        if (!target.has_value()) {
            std::cerr << "Target must be a hexadecimal preferred VA.\n";
            return 2;
        }
        const auto hits = pe.FindRelativeCallReferences(target.value());
        for (const std::uint32_t rva : hits) {
            std::cout << "call-rva=0x" << std::hex << std::uppercase << std::setw(8)
                      << std::setfill('0') << rva
                      << " call-va=0x" << std::setw(8) << pe.imageBase() + rva
                      << " target-va=0x" << std::setw(8) << target.value() << '\n';
        }
        std::cout << "hits=" << std::dec << hits.size() << '\n';
        return 0;
    }

    PrintUsage();
    return 2;
}
