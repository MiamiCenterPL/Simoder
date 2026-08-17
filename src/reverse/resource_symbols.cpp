#include "reverse/resource_symbols.hpp"

#include "core/sha256.hpp"
#include "memory/signature.hpp"

#include <Windows.h>

#include <cstddef>
#include <cstdio>
#include <span>
#include <string_view>
#include <vector>

namespace sc13::reverse {
namespace {

constexpr std::string_view kSupportedFileSha256 =
    "4E5F136A7BF58A52C4B97ED9C9A615A118D4A696E13AA8CAA6A0A51FD4516D35";
constexpr std::string_view kSupportedTextSha256 =
    "D187C19201972ABEBCAA2FA5D8E38263D7A0B0FC3B6803D646636AFB14C49D39";
constexpr std::uint32_t kExpectedPFRecordReadConstructorRva = 0x0052B9B0U;
constexpr std::string_view kPFRecordReadConstructorSignature =
    "53 55 56 8B F1 57 C7 06 44 A8 CF 00 33 C0 8D 4E 04 87 01 "
    "8B 44 24 18 C7 06 5C 58 D3 00 C7 46 08 91 A8 E4 12";
constexpr std::uint32_t kExpectedPublishParsedResourceRva = 0x000099F0U;
constexpr std::string_view kPublishParsedResourceSignature =
    "56 8B 74 24 08 81 7E 0C 04 B1 B1 00 75 19 E8 ?? ?? ?? ?? "
    "8B 4E 10 8B 10 8B 52 34 51 8B 4E 08 51 56 8B C8 FF D2";
constexpr std::uint32_t kExpectedStreamReadRva = 0x004E9630U;
constexpr std::string_view kStreamReadSignature =
    "56 8B F1 8B 46 04 83 F8 FF 74 30 8B 54 24 0C 6A 00 8D 4C 24 10 "
    "51 8B 4C 24 10 52 51 50 FF 15 ?? ?? ?? ?? 85 C0 74 08 8B 44 24 0C "
    "5E C2 08 00";
constexpr std::uint32_t kExpectedPropDeserializeRva = 0x00009710U;
constexpr std::string_view kPropDeserializeSignature =
    "83 EC 10 53 55 8B 6C 24 1C 56 57 33 F6 56 8D 44 24 14 50 55 8B F9 "
    "E8 ?? ?? ?? ?? 8A D8 8B 44 24 1C 83 C4 0C 84 DB 74 5E";

/** Resolves one signature uniquely inside loaded .text and validates its known RVA. */
[[nodiscard]] bool ResolveExactSymbol(
    std::span<const std::byte> text,
    const std::byte* imageBase,
    std::uint32_t textRva,
    std::string_view name,
    std::string_view signatureText,
    std::uint32_t expectedRva,
    void*& address,
    std::uint32_t& rva,
    std::string& error) {
    std::string signatureError;
    const auto signature = memory::Signature::Parse(signatureText, signatureError);
    if (!signature.has_value()) {
        error = "Internal " + std::string(name) + " signature is invalid: " + signatureError;
        return false;
    }
    const memory::UniqueSignatureResult match =
        memory::FindUnique(text, signature.value());
    if (match.status != memory::UniqueSignatureStatus::Matched) {
        error = std::string(name) + " signature resolved to " +
                std::to_string(match.matchCount) + " matches; exactly one is required";
        return false;
    }
    const std::uint32_t resolvedRva =
        textRva + static_cast<std::uint32_t>(match.offset);
    if (resolvedRva != expectedRva) {
        char buffer[9]{};
        std::snprintf(buffer, sizeof(buffer), "%08X", resolvedRva);
        error = std::string(name) + " signature resolved at unexpected RVA 0x" + buffer;
        return false;
    }
    rva = resolvedRva;
    address = const_cast<std::byte*>(imageBase + resolvedRva);
    return true;
}

/** Validates the exact build and exposes its bounded loaded executable code section. */
[[nodiscard]] bool PrepareLoadedText(
    const BuildFingerprint& fingerprint,
    const std::byte*& imageBase,
    std::span<const std::byte>& text,
    std::string& error) {
    if (core::ToHex(fingerprint.fileSha256) != kSupportedFileSha256 ||
        core::ToHex(fingerprint.loadedTextSha256) != kSupportedTextSha256) {
        error = "Resource symbols are not validated for this file/.text fingerprint";
        return false;
    }
    HMODULE executable = GetModuleHandleW(nullptr);
    if (executable == nullptr || fingerprint.textRva == 0U || fingerprint.textSize == 0U) {
        error = "Loaded SimCity executable or .text metadata is unavailable";
        return false;
    }
    imageBase = reinterpret_cast<const std::byte*>(executable);
    text = std::span<const std::byte>(
        imageBase + fingerprint.textRva, fingerprint.textSize);
    return true;
}

}  // namespace

bool ResolvePropDeserializeSymbol(
    const BuildFingerprint& fingerprint,
    PropDeserializeSymbol& symbol,
    std::string& error) noexcept {
    try {
        symbol = {};
        const std::byte* imageBase = nullptr;
        std::span<const std::byte> text;
        if (!PrepareLoadedText(fingerprint, imageBase, text, error)) {
            return false;
        }
        return ResolveExactSymbol(
            text, imageBase, fingerprint.textRva, "PROP deserializer",
            kPropDeserializeSignature, kExpectedPropDeserializeRva,
            symbol.address, symbol.rva, error);
    } catch (...) {
        symbol = {};
        error = "PROP deserializer resolution failed due to an allocation exception";
        return false;
    }
}

bool ResolveResourceSymbols(
    const BuildFingerprint& fingerprint,
    ResourceSymbols& symbols,
    std::string& error) noexcept {
    try {
        symbols = {};
        const std::byte* imageBase = nullptr;
        std::span<const std::byte> text;
        if (!PrepareLoadedText(fingerprint, imageBase, text, error)) {
            return false;
        }
        return ResolveExactSymbol(
                   text, imageBase, fingerprint.textRva, "PFRecordRead constructor",
                   kPFRecordReadConstructorSignature, kExpectedPFRecordReadConstructorRva,
                   symbols.pfRecordReadConstructor,
                   symbols.pfRecordReadConstructorRva, error) &&
               ResolveExactSymbol(
                   text, imageBase, fingerprint.textRva, "parsed-resource publisher",
                   kPublishParsedResourceSignature, kExpectedPublishParsedResourceRva,
                   symbols.publishParsedResource,
                   symbols.publishParsedResourceRva, error) &&
               ResolveExactSymbol(
                   text, imageBase, fingerprint.textRva, "stream read wrapper",
                   kStreamReadSignature, kExpectedStreamReadRva,
                   symbols.streamRead, symbols.streamReadRva, error) &&
               ResolveExactSymbol(
                   text, imageBase, fingerprint.textRva, "PROP deserializer",
                   kPropDeserializeSignature, kExpectedPropDeserializeRva,
                   symbols.propDeserialize, symbols.propDeserializeRva, error);
    } catch (...) {
        symbols = {};
        error = "Resource symbol resolution failed due to an allocation exception";
        return false;
    }
}

}  // namespace sc13::reverse
