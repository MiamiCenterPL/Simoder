#include "core/tgi.hpp"
#include "core/patched_resource_cache.hpp"
#include "core/resource_filter.hpp"
#include "core/runtime_property_table.hpp"
#include "formats/dbpf/dbpf_reader.hpp"
#include "formats/dbpf/refpack.hpp"
#include "formats/prop/prop_document.hpp"
#include "memory/signature.hpp"
#include "reverse/pe_file.hpp"

#include <Windows.h>

#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <span>
#include <string>
#include <vector>

namespace {

int g_failures = 0;

/** Provides a stable non-inlined destination for PE relative-call discovery. */
__declspec(noinline) int PeCallProbeTarget(int value) {
    return value * 7 + 3;
}

/** Emits a direct call whose result prevents tail-call optimization. */
__declspec(noinline) int PeCallProbeCaller(int value) {
    return PeCallProbeTarget(value) + 11;
}

/** Records a failed test expectation with its source expression. */
void Expect(bool condition, const char* expression) {
    if (!condition) {
        ++g_failures;
        std::cerr << "FAILED: " << expression << '\n';
    }
}

#define SC13_EXPECT(expression) Expect((expression), #expression)

/** Appends one big-endian 16-bit integer. */
void AppendBigU16(std::vector<std::byte>& bytes, std::uint16_t value) {
    bytes.push_back(static_cast<std::byte>((value >> 8U) & 0xFFU));
    bytes.push_back(static_cast<std::byte>(value & 0xFFU));
}

/** Appends one big-endian 32-bit integer. */
void AppendBigU32(std::vector<std::byte>& bytes, std::uint32_t value) {
    bytes.push_back(static_cast<std::byte>((value >> 24U) & 0xFFU));
    bytes.push_back(static_cast<std::byte>((value >> 16U) & 0xFFU));
    bytes.push_back(static_cast<std::byte>((value >> 8U) & 0xFFU));
    bytes.push_back(static_cast<std::byte>(value & 0xFFU));
}

/** Writes one little-endian 16-bit integer into a synthetic DBPF fixture. */
void WriteLittleU16(std::vector<std::byte>& bytes, std::size_t offset, std::uint16_t value) {
    bytes[offset] = static_cast<std::byte>(value & 0xFFU);
    bytes[offset + 1] = static_cast<std::byte>((value >> 8U) & 0xFFU);
}

/** Writes one little-endian 32-bit integer into a synthetic DBPF fixture. */
void WriteLittleU32(std::vector<std::byte>& bytes, std::size_t offset, std::uint32_t value) {
    bytes[offset] = static_cast<std::byte>(value & 0xFFU);
    bytes[offset + 1] = static_cast<std::byte>((value >> 8U) & 0xFFU);
    bytes[offset + 2] = static_cast<std::byte>((value >> 16U) & 0xFFU);
    bytes[offset + 3] = static_cast<std::byte>((value >> 24U) & 0xFFU);
}

/** Validates formatting and strict parsing of TGI identifiers. */
void TestTgi() {
    const sc13::core::Tgi expected{0x00B1B104U, 0x61EFC000U, 0x719436BDU};
    SC13_EXPECT(sc13::core::ToString(expected) == "00B1B104:61EFC000:719436BD");
    const auto parsed = sc13::core::ParseTgi("0x00B1B104:61EFC000:719436BD");
    SC13_EXPECT(parsed.has_value());
    SC13_EXPECT(parsed.value() == expected);
    SC13_EXPECT(!sc13::core::ParseTgi("00B1B104:61EFC000").has_value());
}

/** Validates optional semantic TGI matching without dependence on runtime field order. */
void TestResourceFilter() {
    const sc13::core::Tgi target{0x00B1B104U, 0x61EFC000U, 0x719436BDU};
    const sc13::core::ResourceFilter exact{
        target.type, target.group, target.instance};
    SC13_EXPECT(exact.Matches(target));
    SC13_EXPECT(!exact.Matches(sc13::core::Tgi{target.type, target.group, 0x11111111U}));
    const sc13::core::ResourceFilter groupOnly{std::nullopt, target.group, std::nullopt};
    SC13_EXPECT(groupOnly.Matches(target));
    SC13_EXPECT(!groupOnly.Matches(sc13::core::Tgi{target.type, 0x22222222U, target.instance}));
}

/** Validates wildcard signature parsing and all-match scanning. */
void TestSignature() {
    std::string error;
    const auto signature = sc13::memory::Signature::Parse("8B ?? 89", error);
    SC13_EXPECT(signature.has_value());
    const std::vector<std::byte> bytes{
        std::byte{0x8B}, std::byte{0x01}, std::byte{0x89}, std::byte{0x90},
        std::byte{0x8B}, std::byte{0xFF}, std::byte{0x89}};
    const auto matches = sc13::memory::FindAll(bytes, signature.value());
    SC13_EXPECT(matches.size() == 2);
    SC13_EXPECT(matches[0] == 0);
    SC13_EXPECT(matches[1] == 4);
    const auto ambiguous = sc13::memory::FindUnique(bytes, signature.value());
    SC13_EXPECT(ambiguous.status == sc13::memory::UniqueSignatureStatus::Ambiguous);
    SC13_EXPECT(ambiguous.matchCount == 2U);
    const auto absent = sc13::memory::Signature::Parse("DE AD BE EF", error);
    SC13_EXPECT(absent.has_value());
    SC13_EXPECT(sc13::memory::FindAll(bytes, absent.value()).empty());
    const auto noMatch = sc13::memory::FindUnique(bytes, absent.value());
    SC13_EXPECT(noMatch.status == sc13::memory::UniqueSignatureStatus::NotFound);
    SC13_EXPECT(noMatch.matchCount == 0U);

    const std::span<const std::byte> exactBytes(bytes.data(), 3U);
    const auto exact = sc13::memory::FindUnique(exactBytes, signature.value());
    SC13_EXPECT(exact.status == sc13::memory::UniqueSignatureStatus::Matched);
    SC13_EXPECT(exact.offset == 0U);
}

/** Validates runtime-table structure, float reads, copy isolation, and one-field patching. */
void TestRuntimePropertyTable() {
    constexpr std::uint32_t floatMetadata = 0x000D0000U;
    std::array<sc13::core::RuntimePropertyRecord, 4> source{
        sc13::core::RuntimePropertyRecord{
            0x01000000U, std::bit_cast<std::uint32_t>(1.0F), {}, floatMetadata},
        sc13::core::RuntimePropertyRecord{
            0x09AE19D7U, std::bit_cast<std::uint32_t>(300.0F), {}, floatMetadata},
        sc13::core::RuntimePropertyRecord{
            0x0AFB9882U, std::bit_cast<std::uint32_t>(72.0F), {}, floatMetadata},
        sc13::core::RuntimePropertyRecord{
            0x0FD16C15U, std::bit_cast<std::uint32_t>(200.0F), {}, floatMetadata}};
    SC13_EXPECT(sc13::core::ValidateRuntimePropertyTable(source, source.size()) ==
                sc13::core::RuntimeTableStatus::Valid);
    SC13_EXPECT(sc13::core::ReadRuntimeFloat(source, 0x09AE19D7U).value() == 300.0F);

    auto ownedCopy = source;
    SC13_EXPECT(sc13::core::PatchRuntimeFloat(
                    ownedCopy, ownedCopy.size(), 0x09AE19D7U, 300.0F, 345.0F) ==
                sc13::core::RuntimeFloatPatchStatus::Applied);
    SC13_EXPECT(sc13::core::ReadRuntimeFloat(source, 0x09AE19D7U).value() == 300.0F);
    SC13_EXPECT(sc13::core::ReadRuntimeFloat(ownedCopy, 0x09AE19D7U).value() == 345.0F);
    for (std::size_t index = 0; index < source.size(); ++index) {
        if (index == 1U) {
            SC13_EXPECT(source[index].id == ownedCopy[index].id);
            SC13_EXPECT(source[index].auxiliary == ownedCopy[index].auxiliary);
            SC13_EXPECT(source[index].metadata == ownedCopy[index].metadata);
            SC13_EXPECT(source[index].valueBits != ownedCopy[index].valueBits);
        } else {
            SC13_EXPECT(std::memcmp(&source[index], &ownedCopy[index], sizeof(source[index])) == 0);
        }
    }
    SC13_EXPECT(sc13::core::PatchRuntimeFloat(
                    ownedCopy, ownedCopy.size(), 0x09AE19D7U, 300.0F, 345.0F) ==
                sc13::core::RuntimeFloatPatchStatus::AlreadyApplied);
    SC13_EXPECT(sc13::core::PatchRuntimeFloat(
                    source, source.size(), 0x09AE19D7U, 301.0F, 345.0F) ==
                sc13::core::RuntimeFloatPatchStatus::UnexpectedValue);
    SC13_EXPECT(sc13::core::ValidateRuntimePropertyTable(source, source.size() + 1U) ==
                sc13::core::RuntimeTableStatus::WrongCount);
    SC13_EXPECT(sc13::core::PatchRuntimeFloat(
                    source, source.size(), 0x11111111U, 0.0F, 1.0F) ==
                sc13::core::RuntimeFloatPatchStatus::PropertyMissing);

    auto wrongType = source;
    wrongType[1].metadata = 0x000C0000U;
    SC13_EXPECT(sc13::core::PatchRuntimeFloat(
                    wrongType, wrongType.size(), 0x09AE19D7U, 300.0F, 345.0F) ==
                sc13::core::RuntimeFloatPatchStatus::PropertyNotFloat);

    auto malformed = source;
    std::swap(malformed[1], malformed[2]);
    SC13_EXPECT(sc13::core::ValidateRuntimePropertyTable(malformed, malformed.size()) ==
                sc13::core::RuntimeTableStatus::NotStrictlySorted);
}

/** Validates stable cache ownership, reuse, and rejection of unrelated source drift. */
void TestPatchedResourceCache() {
    constexpr std::uint32_t floatMetadata = 0x000D0000U;
    const sc13::core::Tgi tgi{0x00B1B104U, 0x61EFC000U, 0x719436BDU};
    std::array<sc13::core::RuntimePropertyRecord, 3> source{
        sc13::core::RuntimePropertyRecord{
            0x09AE19D7U, std::bit_cast<std::uint32_t>(300.0F), {}, floatMetadata},
        sc13::core::RuntimePropertyRecord{
            0x0AFB9882U, std::bit_cast<std::uint32_t>(72.0F), {}, floatMetadata},
        sc13::core::RuntimePropertyRecord{
            0x0FD16C15U, std::bit_cast<std::uint32_t>(200.0F), {}, floatMetadata}};
    const sc13::core::RuntimeFloatPatch patch{0x09AE19D7U, 300.0F, 345.0F};
    sc13::core::PatchedResourceCache cache;
    sc13::core::PatchedCacheStatus status{};
    std::string error;
    const sc13::core::RuntimeResourceCopy* const created =
        cache.GetOrCreate(tgi, source, source.size(), patch, status, error);
    SC13_EXPECT(created != nullptr);
    SC13_EXPECT(status == sc13::core::PatchedCacheStatus::Created);
    SC13_EXPECT(cache.size() == 1U);
    SC13_EXPECT(sc13::core::ReadRuntimeFloat(source, patch.identifier).value() == 300.0F);
    SC13_EXPECT(sc13::core::ReadRuntimeFloat(created->records(), patch.identifier).value() ==
                345.0F);

    const sc13::core::RuntimeResourceCopy* const reused =
        cache.GetOrCreate(tgi, source, source.size(), patch, status, error);
    SC13_EXPECT(reused == created);
    SC13_EXPECT(status == sc13::core::PatchedCacheStatus::Reused);

    auto alreadyPatched = source;
    alreadyPatched[0].valueBits = std::bit_cast<std::uint32_t>(345.0F);
    const sc13::core::RuntimeResourceCopy* const reusedPatched =
        cache.GetOrCreate(
            tgi, alreadyPatched, alreadyPatched.size(), patch, status, error);
    SC13_EXPECT(reusedPatched == created);
    SC13_EXPECT(status == sc13::core::PatchedCacheStatus::Reused);

    auto drifted = source;
    drifted[1].valueBits = std::bit_cast<std::uint32_t>(73.0F);
    SC13_EXPECT(cache.GetOrCreate(
                    tgi, drifted, drifted.size(), patch, status, error) == nullptr);
    SC13_EXPECT(cache.size() == 1U);

    sc13::core::PatchedResourceCache invalidCache;
    SC13_EXPECT(invalidCache.GetOrCreate(
                    tgi, source, source.size() + 1U, patch, status, error) == nullptr);
    SC13_EXPECT(invalidCache.size() == 0U);
    cache.Clear();
    SC13_EXPECT(cache.size() == 0U);
}

/** Validates literal-only RefPack decoding and truncated-input rejection. */
void TestRefPack() {
    const std::vector<std::byte> compressed{
        std::byte{0x10}, std::byte{0xFB}, std::byte{0x00}, std::byte{0x00}, std::byte{0x04},
        std::byte{0xE0}, std::byte{0x11}, std::byte{0x22}, std::byte{0x33}, std::byte{0x44},
        std::byte{0xFC}};
    std::vector<std::byte> output;
    std::string error;
    SC13_EXPECT(sc13::formats::dbpf::DecompressRefPack(compressed, 4, output, error));
    SC13_EXPECT(output == std::vector<std::byte>(
                              {std::byte{0x11}, std::byte{0x22}, std::byte{0x33}, std::byte{0x44}}));
    SC13_EXPECT(!sc13::formats::dbpf::DecompressRefPack(
        std::span<const std::byte>(compressed.data(), 7), 4, output, error));
}

/** Builds and validates a byte-preserving synthetic PROP document. */
void TestProp() {
    std::vector<std::byte> bytes;
    AppendBigU32(bytes, 3);
    AppendBigU32(bytes, 0x09AE19D7U);
    AppendBigU16(bytes, sc13::formats::prop::kTypeFloat);
    AppendBigU16(bytes, 0x8000U);
    AppendBigU32(bytes, std::bit_cast<std::uint32_t>(300.0F));
    AppendBigU32(bytes, 0xDEADBEEFU);
    AppendBigU16(bytes, sc13::formats::prop::kTypeUInt32);
    AppendBigU16(bytes, 0x8000U);
    AppendBigU32(bytes, 0x12345678U);
    AppendBigU32(bytes, 0xCAFEBABEU);
    AppendBigU16(bytes, 0x007FU);
    AppendBigU16(bytes, 0x8020U);
    AppendBigU32(bytes, 2);
    AppendBigU32(bytes, 3);
    bytes.insert(bytes.end(), {
        std::byte{0x10}, std::byte{0x20}, std::byte{0x30},
        std::byte{0x40}, std::byte{0x50}, std::byte{0x60}});

    const std::vector<std::byte> original = bytes;
    sc13::formats::prop::PropDocument document;
    std::string error;
    SC13_EXPECT(document.Parse(bytes, error));
    SC13_EXPECT(std::vector<std::byte>(document.bytes().begin(), document.bytes().end()) == original);
    const auto* opaque = document.Find(0xCAFEBABEU);
    SC13_EXPECT(opaque != nullptr);
    SC13_EXPECT(opaque->isArray);
    SC13_EXPECT(opaque->type == 0x7FU);
    SC13_EXPECT(opaque->valueSize == 6);
    SC13_EXPECT(document.GetFloat(0x09AE19D7U).value() == 300.0F);
    SC13_EXPECT(document.SetFloat(0x09AE19D7U, 345.0F, error));
    SC13_EXPECT(document.GetFloat(0x09AE19D7U).value() == 345.0F);
    SC13_EXPECT(document.bytes()[12] == std::byte{0x43});
    SC13_EXPECT(document.bytes()[13] == std::byte{0xAC});
    SC13_EXPECT(document.bytes()[14] == std::byte{0x80});
    SC13_EXPECT(document.bytes()[15] == std::byte{0x00});
    for (std::size_t index = 0; index < document.bytes().size(); ++index) {
        if (index < 12 || index > 15) {
            SC13_EXPECT(document.bytes()[index] == original[index]);
        }
    }
}

/** Creates a minimal uncompressed DBPF v3 package containing one PROP resource. */
[[nodiscard]] std::vector<std::byte> BuildSyntheticDbpf(
    const sc13::core::Tgi& tgi, std::span<const std::byte> resource, bool compressed) {
    constexpr std::size_t headerSize = 96;
    constexpr std::size_t indexEntrySize = 32;
    std::vector<std::byte> stored(resource.begin(), resource.end());
    if (compressed) {
        SC13_EXPECT(resource.size() == 4);
        stored = {
            std::byte{0x10}, std::byte{0xFB}, std::byte{0x00}, std::byte{0x00}, std::byte{0x04},
            std::byte{0xE0}, resource[0], resource[1], resource[2], resource[3], std::byte{0xFC}};
    }
    const std::size_t indexOffset = headerSize + stored.size();
    std::vector<std::byte> bytes(indexOffset + 4 + indexEntrySize);
    bytes[0] = std::byte{'D'};
    bytes[1] = std::byte{'B'};
    bytes[2] = std::byte{'P'};
    bytes[3] = std::byte{'F'};
    WriteLittleU32(bytes, 0x04, 3);
    WriteLittleU32(bytes, 0x24, 1);
    WriteLittleU32(bytes, 0x2C, 4 + indexEntrySize);
    WriteLittleU32(bytes, 0x3C, 3);
    WriteLittleU32(bytes, 0x40, static_cast<std::uint32_t>(indexOffset));
    std::copy(stored.begin(), stored.end(), bytes.begin() + headerSize);

    std::size_t cursor = indexOffset;
    WriteLittleU32(bytes, cursor, 0);
    cursor += 4;
    WriteLittleU32(bytes, cursor, tgi.type);
    cursor += 4;
    WriteLittleU32(bytes, cursor, tgi.group);
    cursor += 4;
    WriteLittleU32(bytes, cursor, 0);
    cursor += 4;
    WriteLittleU32(bytes, cursor, tgi.instance);
    cursor += 4;
    WriteLittleU32(bytes, cursor, headerSize);
    cursor += 4;
    WriteLittleU32(bytes, cursor, static_cast<std::uint32_t>(stored.size()));
    cursor += 4;
    WriteLittleU32(bytes, cursor, static_cast<std::uint32_t>(resource.size()));
    cursor += 4;
    WriteLittleU16(bytes, cursor, compressed ? 0xFFFFU : 0x0000U);
    WriteLittleU16(bytes, cursor + 2, 1);
    return bytes;
}

/** Validates DBPF header, index, TGI lookup, and exact extraction. */
void TestDbpf() {
    const sc13::core::Tgi tgi{0x00B1B104U, 0x61EFC000U, 0x719436BDU};
    const std::vector<std::byte> resource{
        std::byte{0x00}, std::byte{0x00}, std::byte{0x00}, std::byte{0x00}};
    const auto packageBytes = BuildSyntheticDbpf(tgi, resource, false);

    wchar_t temporaryDirectory[MAX_PATH]{};
    wchar_t temporaryFile[MAX_PATH]{};
    SC13_EXPECT(GetTempPathW(MAX_PATH, temporaryDirectory) != 0);
    SC13_EXPECT(GetTempFileNameW(temporaryDirectory, L"s13", 0, temporaryFile) != 0);
    {
        std::ofstream stream(temporaryFile, std::ios::binary | std::ios::trunc);
        stream.write(
            reinterpret_cast<const char*>(packageBytes.data()),
            static_cast<std::streamsize>(packageBytes.size()));
        SC13_EXPECT(static_cast<bool>(stream));
    }

    sc13::formats::dbpf::DbpfPackage package;
    std::string error;
    SC13_EXPECT(package.Open(temporaryFile, error));
    const auto* entry = package.Find(tgi);
    SC13_EXPECT(entry != nullptr);
    std::vector<std::byte> extracted;
    SC13_EXPECT(package.ReadResource(*entry, extracted, error));
    SC13_EXPECT(extracted == resource);

    const auto compressedPackageBytes = BuildSyntheticDbpf(tgi, resource, true);
    {
        std::ofstream stream(temporaryFile, std::ios::binary | std::ios::trunc);
        stream.write(
            reinterpret_cast<const char*>(compressedPackageBytes.data()),
            static_cast<std::streamsize>(compressedPackageBytes.size()));
        SC13_EXPECT(static_cast<bool>(stream));
    }
    SC13_EXPECT(package.Open(temporaryFile, error));
    entry = package.Find(tgi);
    SC13_EXPECT(entry != nullptr);
    SC13_EXPECT(entry->isCompressed());
    SC13_EXPECT(package.ReadResource(*entry, extracted, error));
    SC13_EXPECT(extracted == resource);
    SC13_EXPECT(DeleteFileW(temporaryFile) != FALSE);
}

/** Validates PE32 parsing against the currently executing x86 test image. */
void TestPeFile() {
    wchar_t executablePath[32768]{};
    const DWORD length = GetModuleFileNameW(
        nullptr, executablePath, static_cast<DWORD>(std::size(executablePath)));
    SC13_EXPECT(length != 0);
    SC13_EXPECT(length < std::size(executablePath));

    sc13::reverse::PeFile pe;
    std::string error;
    SC13_EXPECT(pe.Open(executablePath, error));
    SC13_EXPECT(pe.machine() == 0x014CU);
    SC13_EXPECT(pe.imageBase() != 0);
    SC13_EXPECT(pe.entryPointRva() != 0);
    SC13_EXPECT(!pe.sections().empty());
    const std::array<std::string_view, 1> needles{"All SC13 tests passed."};
    const auto strings = pe.FindAsciiStrings(needles, 4, 4);
    SC13_EXPECT(!strings.empty());
    SC13_EXPECT(strings.front().rva.has_value());
    const auto moduleBase = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
    const auto targetAddress = reinterpret_cast<std::uintptr_t>(&PeCallProbeTarget);
    SC13_EXPECT(targetAddress >= moduleBase);
    const auto targetRva = static_cast<std::uint32_t>(targetAddress - moduleBase);
    const auto calls = pe.FindRelativeCallReferences(pe.imageBase() + targetRva, 16);
    SC13_EXPECT(!calls.empty());
    SC13_EXPECT(PeCallProbeCaller(2) == 28);
}

}  // namespace

/** Executes deterministic unit tests without a third-party framework. */
int main() {
    TestTgi();
    TestResourceFilter();
    TestSignature();
    TestRuntimePropertyTable();
    TestPatchedResourceCache();
    TestRefPack();
    TestProp();
    TestDbpf();
    TestPeFile();
    if (g_failures == 0) {
        std::cout << "All SC13 tests passed.\n";
        return 0;
    }
    std::cerr << g_failures << " test expectation(s) failed.\n";
    return 1;
}
