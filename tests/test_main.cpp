#include "config/loader_config.hpp"
#include "core/tgi.hpp"
#include "core/patched_resource_cache.hpp"
#include "core/resource_filter.hpp"
#include "core/runtime_property_table.hpp"
#include "formats/dbpf/dbpf_reader.hpp"
#include "formats/dbpf/refpack.hpp"
#include "formats/prop/prop_document.hpp"
#include "formats/toon/toon_document.hpp"
#include "memory/signature.hpp"
#include "logging/log_event.hpp"
#include "mods/mod_definition_parser.hpp"
#include "mods/mod_manager.hpp"
#include "mods/mod_state_store.hpp"
#include "mods/patch_registry.hpp"
#include "reverse/pe_file.hpp"
#include "runtime/runtime_resource_cache.hpp"

#include <Windows.h>

#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <set>
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

/** Validates the strict TOON subset used by M3 manifests, config, and overrides. */
void TestToon() {
    constexpr std::string_view source =
        "id: dawid.better-sanitizer\n"
        "name: Better Sanitizer\n"
        "version: 1.0.0\n"
        "developerConsole:\n"
        "  enabled: false\n"
        "patches[1]:\n"
        "  - target:\n"
        "      type: 0x00B1B104\n"
        "      group: 0x61EFC000\n"
        "      instance: 0x719436BD\n"
        "    properties[1]{id,type,operation,value}:\n"
        "      0x09AE19D7,float,set,345\n";
    sc13::formats::toon::Document document;
    sc13::formats::toon::ParseError error;
    SC13_EXPECT(sc13::formats::toon::Parse(source, document, error));
    const auto id = document.root().find("id");
    SC13_EXPECT(id != document.root().end());
    SC13_EXPECT(id->second.AsString() != nullptr);
    SC13_EXPECT(*id->second.AsString() == "dawid.better-sanitizer");

    const auto developerConsole = document.root().find("developerConsole");
    SC13_EXPECT(developerConsole != document.root().end());
    const auto* enabled = developerConsole->second.Find("enabled");
    SC13_EXPECT(enabled != nullptr);
    SC13_EXPECT(enabled->AsBoolean() != nullptr);
    SC13_EXPECT(!*enabled->AsBoolean());

    const auto patches = document.root().find("patches");
    SC13_EXPECT(patches != document.root().end());
    SC13_EXPECT(patches->second.AsArray() != nullptr);
    SC13_EXPECT(patches->second.AsArray()->size() == 1U);
    const auto* target = patches->second.AsArray()->front().Find("target");
    SC13_EXPECT(target != nullptr);
    const auto* type = target->Find("type");
    SC13_EXPECT(type != nullptr);
    SC13_EXPECT(type->AsString() != nullptr);
    SC13_EXPECT(*type->AsString() == "0x00B1B104");
    const auto* properties = patches->second.AsArray()->front().Find("properties");
    SC13_EXPECT(properties != nullptr);
    SC13_EXPECT(properties->AsArray() != nullptr);
    SC13_EXPECT(properties->AsArray()->front().Find("value")->AsNumber() != nullptr);
    SC13_EXPECT(*properties->AsArray()->front().Find("value")->AsNumber() == 345.0);

    SC13_EXPECT(!sc13::formats::toon::Parse(
        "items[2]: one\n", document, error));
    SC13_EXPECT(error.line == 1U);
    SC13_EXPECT(!sc13::formats::toon::Parse(
        "root:\n   child: bad-indent\n", document, error));
    SC13_EXPECT(error.line == 2U);
    SC13_EXPECT(!sc13::formats::toon::Parse(
        "key: one\nkey: two\n", document, error));
    SC13_EXPECT(error.line == 2U);
}

/** Creates an empty uniquely named temporary directory for filesystem tests. */
[[nodiscard]] std::filesystem::path CreateTemporaryDirectory() {
    wchar_t temporaryDirectory[MAX_PATH]{};
    wchar_t temporaryName[MAX_PATH]{};
    SC13_EXPECT(GetTempPathW(MAX_PATH, temporaryDirectory) != 0);
    SC13_EXPECT(GetTempFileNameW(temporaryDirectory, L"s13", 0, temporaryName) != 0);
    SC13_EXPECT(DeleteFileW(temporaryName) != FALSE);
    SC13_EXPECT(CreateDirectoryW(temporaryName, nullptr) != FALSE);
    return temporaryName;
}

/** Writes one complete UTF-8 fixture file. */
void WriteTextFile(const std::filesystem::path& path, std::string_view text) {
    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    stream.write(text.data(), static_cast<std::streamsize>(text.size()));
    SC13_EXPECT(static_cast<bool>(stream));
}

/** Writes one complete valid mod fixture with a configurable float set operation. */
void WriteModFixture(
    const std::filesystem::path& directory,
    std::string_view id,
    std::string_view name,
    float value) {
    std::error_code directoryError;
    std::filesystem::create_directories(directory, directoryError);
    SC13_EXPECT(!directoryError);
    WriteTextFile(
        directory / L"mod.toon",
        "id: " + std::string(id) + "\n" +
        "name: " + std::string(name) + "\n" +
        "version: 1.0.0\n" +
        "author: Simoder Tests\n");
    WriteTextFile(
        directory / L"overrides.toon",
        "patches[1]:\n"
        "  - target:\n"
        "      type: 0x00B1B104\n"
        "      group: 0x61EFC000\n"
        "      instance: 0x719436BD\n"
        "    properties[1]{id,type,operation,value}:\n"
        "      0x09AE19D7,float,set," + std::to_string(value) + "\n");
}

/** Validates typed manifest and override conversion plus unsafe-input rejection. */
void TestModDefinitionParser() {
    const std::filesystem::path directory = CreateTemporaryDirectory();
    WriteTextFile(
        directory / L"mod.toon",
        "id: dawid.better-sanitizer\n"
        "name: Better Sanitizer\n"
        "version: 1.0.0\n"
        "author: Dawid\n");
    WriteTextFile(
        directory / L"overrides.toon",
        "patches[1]:\n"
        "  - target:\n"
        "      type: 0x00B1B104\n"
        "      group: 0x61EFC000\n"
        "      instance: 0x719436BD\n"
        "    properties[1]{id,type,operation,value}:\n"
        "      0x09AE19D7,float,set,345\n");

    sc13::mods::ModDefinition definition;
    std::string error;
    SC13_EXPECT(sc13::mods::LoadModDefinition(directory, definition, error));
    SC13_EXPECT(definition.manifest.id == "dawid.better-sanitizer");
    SC13_EXPECT(definition.manifest.name == "Better Sanitizer");
    SC13_EXPECT(definition.patches.size() == 1U);
    const sc13::core::Tgi expectedTarget{
        0x00B1B104U, 0x61EFC000U, 0x719436BDU};
    SC13_EXPECT(definition.patches.front().target == expectedTarget);
    SC13_EXPECT(definition.patches.front().properties.size() == 1U);
    SC13_EXPECT(
        definition.patches.front().properties.front().operation ==
        sc13::mods::PatchOperation::Set);
    SC13_EXPECT(definition.patches.front().properties.front().value == 345.0F);

    WriteTextFile(
        directory / L"mod.toon",
        "id: Invalid.Id\nname: Invalid\nversion: 1.0.0\n");
    SC13_EXPECT(!sc13::mods::LoadModDefinition(directory, definition, error));
    SC13_EXPECT(error.find("valid stable mod identity") != std::string::npos);

    WriteTextFile(
        directory / L"mod.toon",
        "id: dawid.valid\nname: Valid\nversion: 1.0.0\n");
    WriteTextFile(
        directory / L"overrides.toon",
        "patches[1]:\n"
        "  - target:\n"
        "      type: 0x00B1B104\n"
        "      group: 0x61EFC000\n"
        "      instance: 0x719436BD\n"
        "    properties[1]{id,type,operation,value}:\n"
        "      0x09AE19D7,float,divide,0\n");
    SC13_EXPECT(!sc13::mods::LoadModDefinition(directory, definition, error));
    SC13_EXPECT(error.find("cannot use zero") != std::string::npos);
    std::error_code removeError;
    std::filesystem::remove_all(directory, removeError);
    SC13_EXPECT(!removeError);
}

/** Creates one typed float patch block for registry tests. */
[[nodiscard]] sc13::mods::ResourcePatchDefinition MakeResourcePatch(
    const sc13::core::Tgi& target,
    std::uint32_t propertyId,
    sc13::mods::PatchOperation operation,
    float value) {
    return sc13::mods::ResourcePatchDefinition{
        target,
        {sc13::mods::PropertyPatch{
            propertyId, sc13::mods::PropertyType::Float, operation, value}}};
}

/** Validates ownership, activation order, conflicts, and vanilla-derived application. */
void TestPatchRegistry() {
    constexpr std::uint32_t floatMetadata = 0x000D0000U;
    const sc13::core::Tgi target{0x00B1B104U, 0x61EFC000U, 0x719436BDU};
    const auto setMaintenance = MakeResourcePatch(
        target, 0x09AE19D7U, sc13::mods::PatchOperation::Set, 345.0F);
    const auto addMaintenance = MakeResourcePatch(
        target, 0x09AE19D7U, sc13::mods::PatchOperation::Add, 5.0F);
    const auto multiplyCapacity = MakeResourcePatch(
        target, 0x0AFB9882U, sc13::mods::PatchOperation::Multiply, 2.0F);

    sc13::mods::PatchRegistry registry;
    std::set<sc13::core::Tgi> affected;
    std::string error;
    SC13_EXPECT(registry.Current()->generation() == 0U);
    SC13_EXPECT(registry.Activate(
        "mod.set", std::span(&setMaintenance, 1U), affected, error));
    SC13_EXPECT(affected == std::set<sc13::core::Tgi>{target});
    const std::shared_ptr<const sc13::mods::PatchRegistrySnapshot> first =
        registry.Current();
    SC13_EXPECT(first->generation() == 1U);
    SC13_EXPECT(first->Find(target) != nullptr);
    SC13_EXPECT(first->Find(target)->front().owner == "mod.set");

    SC13_EXPECT(registry.Activate(
        "mod.add", std::span(&addMaintenance, 1U), affected, error));
    const auto second = registry.Current();
    SC13_EXPECT(second->generation() == 2U);
    SC13_EXPECT(second->Find(target)->size() == 2U);
    SC13_EXPECT((*second->Find(target))[0].owner == "mod.set");
    SC13_EXPECT((*second->Find(target))[1].owner == "mod.add");
    SC13_EXPECT(second->conflicts().size() == 1U);
    SC13_EXPECT(second->conflicts().front().propertyId == 0x09AE19D7U);
    SC13_EXPECT(first->Find(target)->size() == 1U);

    std::array<sc13::core::RuntimePropertyRecord, 2> vanilla{
        sc13::core::RuntimePropertyRecord{
            0x09AE19D7U, std::bit_cast<std::uint32_t>(300.0F), {}, floatMetadata},
        sc13::core::RuntimePropertyRecord{
            0x0AFB9882U, std::bit_cast<std::uint32_t>(72.0F), {}, floatMetadata}};
    std::vector<sc13::core::RuntimePropertyRecord> rebuilt;
    SC13_EXPECT(sc13::mods::ApplyPatchSequence(
        target, vanilla, *second->Find(target), rebuilt, error));
    SC13_EXPECT(sc13::core::ReadRuntimeFloat(rebuilt, 0x09AE19D7U).value() == 350.0F);
    SC13_EXPECT(sc13::core::ReadRuntimeFloat(vanilla, 0x09AE19D7U).value() == 300.0F);

    const auto generationBeforeFailure = second->generation();
    SC13_EXPECT(!registry.Activate(
        "mod.set", std::span(&setMaintenance, 1U), affected, error));
    SC13_EXPECT(registry.Current()->generation() == generationBeforeFailure);
    SC13_EXPECT(registry.Deactivate("mod.add", affected, error));
    SC13_EXPECT(registry.Current()->Find(target)->size() == 1U);

    sc13::mods::PatchRegistry nonConflicting;
    SC13_EXPECT(nonConflicting.Activate(
        "mod.maintenance", std::span(&setMaintenance, 1U), affected, error));
    SC13_EXPECT(nonConflicting.Activate(
        "mod.capacity", std::span(&multiplyCapacity, 1U), affected, error));
    SC13_EXPECT(nonConflicting.Current()->conflicts().empty());
    SC13_EXPECT(sc13::mods::ApplyPatchSequence(
        target, vanilla, *nonConflicting.Current()->Find(target), rebuilt, error));
    SC13_EXPECT(sc13::core::ReadRuntimeFloat(rebuilt, 0x09AE19D7U).value() == 345.0F);
    SC13_EXPECT(sc13::core::ReadRuntimeFloat(rebuilt, 0x0AFB9882U).value() == 144.0F);
}

/** Validates generation retention, targeted invalidation, and vanilla restoration. */
void TestRuntimeResourceCache() {
    constexpr std::uint32_t floatMetadata = 0x000D0000U;
    const sc13::core::Tgi targetA{0x00B1B104U, 0x61EFC000U, 0x719436BDU};
    const sc13::core::Tgi targetB{0x00B1B104U, 0x61EFC000U, 0x11111111U};
    const auto setMaintenance = MakeResourcePatch(
        targetA, 0x09AE19D7U, sc13::mods::PatchOperation::Set, 345.0F);
    sc13::mods::PatchRegistry registry;
    std::set<sc13::core::Tgi> affected;
    std::string error;
    SC13_EXPECT(registry.Activate(
        "mod.cache", std::span(&setMaintenance, 1U), affected, error));
    const auto snapshot = registry.Current();

    std::array<sc13::core::RuntimePropertyRecord, 1> vanillaA{
        sc13::core::RuntimePropertyRecord{
            0x09AE19D7U, std::bit_cast<std::uint32_t>(300.0F), {}, floatMetadata}};
    std::array<sc13::core::RuntimePropertyRecord, 1> vanillaB{
        sc13::core::RuntimePropertyRecord{
            0x09AE19D7U, std::bit_cast<std::uint32_t>(100.0F), {}, floatMetadata}};
    sc13::runtime::RuntimeResourceCache cache;
    sc13::runtime::RuntimeResourceBuildPlan planA;
    const sc13::runtime::RuntimeResourceKey keyA{targetA, 0x1000U};
    SC13_EXPECT(cache.Prepare(
        keyA, vanillaA, snapshot->generation(), *snapshot->Find(targetA), planA, error));
    SC13_EXPECT(
        planA.status == sc13::runtime::RuntimeResourceBuildStatus::Created);
    SC13_EXPECT(sc13::core::ReadRuntimeFloat(planA.desired, 0x09AE19D7U).value() == 345.0F);
    SC13_EXPECT(sc13::core::ReadRuntimeFloat(vanillaA, 0x09AE19D7U).value() == 300.0F);
    SC13_EXPECT(cache.Commit(planA, error));

    sc13::runtime::RuntimeResourceBuildPlan stalePlan;
    SC13_EXPECT(cache.Prepare(
        keyA, planA.desired, snapshot->generation(),
        *snapshot->Find(targetA), stalePlan, error));
    SC13_EXPECT(cache.Invalidate(std::set<sc13::core::Tgi>{targetA}) == 1U);
    SC13_EXPECT(!cache.Commit(stalePlan, error));

    sc13::runtime::RuntimeResourceBuildPlan planB;
    const sc13::runtime::RuntimeResourceKey keyB{targetB, 0x2000U};
    SC13_EXPECT(cache.Prepare(
        keyB, vanillaB, snapshot->generation(), {}, planB, error));
    SC13_EXPECT(cache.Commit(planB, error));
    SC13_EXPECT(cache.size() == 2U);
    SC13_EXPECT(cache.Invalidate(std::set<sc13::core::Tgi>{targetA}) == 0U);

    sc13::runtime::RuntimeResourceBuildPlan reusedB;
    SC13_EXPECT(cache.Prepare(
        keyB, planB.desired, snapshot->generation() + 1U, {}, reusedB, error));
    SC13_EXPECT(
        reusedB.status == sc13::runtime::RuntimeResourceBuildStatus::Reused);

    SC13_EXPECT(registry.Deactivate("mod.cache", affected, error));
    sc13::runtime::RuntimeResourceBuildPlan restoredA;
    SC13_EXPECT(cache.Prepare(
        keyA, planA.desired, registry.Current()->generation(), {}, restoredA, error));
    SC13_EXPECT(
        restoredA.status == sc13::runtime::RuntimeResourceBuildStatus::Restored);
    SC13_EXPECT(
        sc13::core::ReadRuntimeFloat(restoredA.desired, 0x09AE19D7U).value() == 300.0F);
    SC13_EXPECT(cache.Commit(restoredA, error));
    const auto restoreEntries = cache.SnapshotForRestore();
    SC13_EXPECT(restoreEntries.size() == 2U);
    SC13_EXPECT(std::any_of(
        restoreEntries.begin(), restoreEntries.end(),
        [&keyA](const sc13::runtime::RuntimeResourceRestoreEntry& entry) {
            return entry.key == keyA && entry.vanilla.size() == 1U &&
                   entry.lastApplied.size() == 1U;
        }));
}

/** Validates generated defaults and typed DeveloperConsole configuration parsing. */
void TestLoaderConfig() {
    const std::filesystem::path directory = CreateTemporaryDirectory();
    sc13::config::LoaderConfig config;
    std::string error;
    SC13_EXPECT(sc13::config::LoadOrCreateConfig(directory, config, error));
    SC13_EXPECT(!config.developerConsole.enabled);
    SC13_EXPECT(config.developerConsole.captureGameLogs);
    SC13_EXPECT(std::filesystem::is_regular_file(directory / L"config.toon"));

    WriteTextFile(
        directory / L"config.toon",
        "developerConsole:\n"
        "  enabled: true\n"
        "  captureGameLogs: false\n"
        "  captureLoaderLogs: true\n"
        "  captureModLogs: true\n"
        "  level: debug\n");
    SC13_EXPECT(sc13::config::LoadOrCreateConfig(directory, config, error));
    SC13_EXPECT(config.developerConsole.enabled);
    SC13_EXPECT(!config.developerConsole.captureGameLogs);
    SC13_EXPECT(config.developerConsole.level == "debug");

    WriteTextFile(
        directory / L"config.toon",
        "developerConsole:\n"
        "  enabled: yes\n");
    SC13_EXPECT(!sc13::config::LoadOrCreateConfig(directory, config, error));
    SC13_EXPECT(!config.developerConsole.enabled);
    std::error_code removeError;
    std::filesystem::remove_all(directory, removeError);
    SC13_EXPECT(!removeError);
}

/** Validates centralized semantic source labels and severity filtering. */
void TestLogEventClassification() {
    SC13_EXPECT(std::string_view(sc13::logging::SourceTypeName(
                    sc13::logging::SourceType::Game)) == "GAME");
    SC13_EXPECT(std::string_view(sc13::logging::SourceTypeName(
                    sc13::logging::SourceType::Loader)) == "LOADER");
    SC13_EXPECT(std::string_view(sc13::logging::SourceTypeName(
                    sc13::logging::SourceType::Mod)) == "MOD");
    sc13::logging::Level level = sc13::logging::Level::Error;
    SC13_EXPECT(sc13::logging::ParseLevel("debug", level));
    SC13_EXPECT(level == sc13::logging::Level::Debug);
    SC13_EXPECT(sc13::logging::PassesMinimum(
        sc13::logging::Level::Warning, sc13::logging::Level::Info));
    SC13_EXPECT(!sc13::logging::PassesMinimum(
        sc13::logging::Level::Trace, sc13::logging::Level::Info));
    SC13_EXPECT(!sc13::logging::ParseLevel("verbose", level));
}

/** Validates atomic enabled-state persistence, ordering, and strict validation. */
void TestModStateStore() {
    const std::filesystem::path directory = CreateTemporaryDirectory();
    const std::filesystem::path statePath = directory / L"state.toon";
    std::vector<sc13::mods::ModId> enabled;
    std::string error;
    SC13_EXPECT(sc13::mods::LoadEnabledState(statePath, enabled, error));
    SC13_EXPECT(enabled.empty());

    const std::array<sc13::mods::ModId, 2> expected{
        "example.first", "example.second"};
    SC13_EXPECT(sc13::mods::SaveEnabledState(statePath, expected, error));
    SC13_EXPECT(!std::filesystem::exists(directory / L"state.toon.tmp"));
    SC13_EXPECT(sc13::mods::LoadEnabledState(statePath, enabled, error));
    SC13_EXPECT(enabled.size() == expected.size());
    SC13_EXPECT(enabled[0] == expected[0]);
    SC13_EXPECT(enabled[1] == expected[1]);

    WriteTextFile(statePath, "enabled[2]: duplicate.id,duplicate.id\n");
    SC13_EXPECT(!sc13::mods::LoadEnabledState(statePath, enabled, error));
    SC13_EXPECT(enabled.empty());

    const std::array<sc13::mods::ModId, 2> invalid{
        "valid.id", "Invalid ID"};
    SC13_EXPECT(!sc13::mods::SaveEnabledState(statePath, invalid, error));
    std::error_code removeError;
    std::filesystem::remove_all(directory, removeError);
    SC13_EXPECT(!removeError);
}

/** Validates discovery, persistence, changed files, removal, and failure isolation. */
void TestModManager() {
    const std::filesystem::path root = CreateTemporaryDirectory();
    const std::filesystem::path modsDirectory = root / L"mods";
    const std::filesystem::path statePath = root / L"simoder" / L"state.toon";
    const std::filesystem::path sanitizer = modsDirectory / L"BetterSanitizer";
    WriteModFixture(
        sanitizer, "dawid.better-sanitizer", "Better Sanitizer", 345.0F);

    sc13::mods::PatchRegistry registry;
    sc13::runtime::RuntimeResourceCache cache;
    std::size_t runtimeRefreshCount = 0U;
    std::set<sc13::core::Tgi> lastRuntimeRefresh;
    sc13::mods::ModManager manager(
        modsDirectory, statePath, registry, cache,
        [&runtimeRefreshCount, &lastRuntimeRefresh](
            const std::set<sc13::core::Tgi>& affected,
            std::string& refreshError) {
            ++runtimeRefreshCount;
            lastRuntimeRefresh = affected;
            refreshError.clear();
            return true;
        });
    std::string error;
    SC13_EXPECT(manager.Initialize(error));
    auto entries = manager.Snapshot();
    SC13_EXPECT(entries.size() == 1U);
    SC13_EXPECT(entries.front().state == sc13::mods::ModState::Inactive);
    SC13_EXPECT(entries.front().canEnable);

    SC13_EXPECT(manager.SetEnabled("dawid.better-sanitizer", true, error));
    SC13_EXPECT(registry.IsActive("dawid.better-sanitizer"));
    SC13_EXPECT(manager.EnabledOrder().size() == 1U);
    const sc13::core::Tgi target{0x00B1B104U, 0x61EFC000U, 0x719436BDU};
    SC13_EXPECT(runtimeRefreshCount == 1U);
    SC13_EXPECT(lastRuntimeRefresh.contains(target));
    SC13_EXPECT(
        registry.Current()->Find(target)->front().patch.properties.front().value ==
        345.0F);

    {
        sc13::mods::PatchRegistry restartedRegistry;
        sc13::runtime::RuntimeResourceCache restartedCache;
        sc13::mods::ModManager restarted(
            modsDirectory, statePath, restartedRegistry, restartedCache);
        SC13_EXPECT(restarted.Initialize(error));
        SC13_EXPECT(restartedRegistry.IsActive("dawid.better-sanitizer"));
        SC13_EXPECT(restarted.EnabledOrder().size() == 1U);
    }

    WriteModFixture(
        sanitizer, "dawid.better-sanitizer", "Better Sanitizer", 360.0F);
    SC13_EXPECT(manager.Refresh(error));
    entries = manager.Snapshot();
    SC13_EXPECT(entries.front().state == sc13::mods::ModState::Changed);
    SC13_EXPECT(entries.front().changed);
    SC13_EXPECT(
        registry.Current()->Find(target)->front().patch.properties.front().value ==
        345.0F);

    WriteTextFile(sanitizer / L"overrides.toon", "patches: malformed\n");
    SC13_EXPECT(manager.Refresh(error));
    entries = manager.Snapshot();
    SC13_EXPECT(entries.front().state == sc13::mods::ModState::Changed);
    SC13_EXPECT(registry.IsActive("dawid.better-sanitizer"));
    SC13_EXPECT(
        registry.Current()->Find(target)->front().patch.properties.front().value ==
        345.0F);

    WriteModFixture(
        sanitizer, "dawid.better-sanitizer", "Better Sanitizer", 360.0F);
    SC13_EXPECT(manager.Refresh(error));

    SC13_EXPECT(manager.SetEnabled("dawid.better-sanitizer", false, error));
    SC13_EXPECT(!registry.IsActive("dawid.better-sanitizer"));
    SC13_EXPECT(manager.SetEnabled("dawid.better-sanitizer", true, error));
    SC13_EXPECT(
        registry.Current()->Find(target)->front().patch.properties.front().value ==
        360.0F);

    SC13_EXPECT(manager.SetEnabled("dawid.better-sanitizer", false, error));
    WriteModFixture(
        sanitizer, "dawid.better-sanitizer", "Better Sanitizer", 400.0F);
    SC13_EXPECT(manager.SetEnabled("dawid.better-sanitizer", true, error));
    SC13_EXPECT(runtimeRefreshCount == 5U);
    SC13_EXPECT(
        registry.Current()->Find(target)->front().patch.properties.front().value ==
        400.0F);

    WriteTextFile(sanitizer / L"overrides.toon", "patches: malformed\n");
    SC13_EXPECT(manager.Refresh(error));
    SC13_EXPECT(manager.SetEnabled("dawid.better-sanitizer", false, error));
    entries = manager.Snapshot();
    SC13_EXPECT(entries.front().state == sc13::mods::ModState::Failed);
    SC13_EXPECT(!manager.SetEnabled("dawid.better-sanitizer", true, error));
    WriteModFixture(
        sanitizer, "dawid.better-sanitizer", "Better Sanitizer", 360.0F);
    SC13_EXPECT(manager.Refresh(error));
    SC13_EXPECT(manager.SetEnabled("dawid.better-sanitizer", true, error));

    const std::filesystem::path invalid = modsDirectory / L"BrokenMod";
    std::error_code directoryError;
    std::filesystem::create_directories(invalid, directoryError);
    SC13_EXPECT(!directoryError);
    WriteTextFile(invalid / L"mod.toon", "id: Invalid ID\n");
    SC13_EXPECT(manager.Refresh(error));
    entries = manager.Snapshot();
    SC13_EXPECT(entries.size() == 2U);
    SC13_EXPECT(registry.IsActive("dawid.better-sanitizer"));

    std::filesystem::remove_all(sanitizer, directoryError);
    SC13_EXPECT(!directoryError);
    SC13_EXPECT(manager.Refresh(error));
    entries = manager.Snapshot();
    SC13_EXPECT(!registry.IsActive("dawid.better-sanitizer"));
    SC13_EXPECT(manager.EnabledOrder().empty());
    SC13_EXPECT(std::any_of(
        entries.begin(), entries.end(), [](const sc13::mods::ModUiEntry& entry) {
            return entry.id == "dawid.better-sanitizer" &&
                   entry.state == sc13::mods::ModState::Missing;
        }));

    WriteModFixture(
        modsDirectory / L"DuplicateOne", "test.duplicate", "Duplicate One", 410.0F);
    WriteModFixture(
        modsDirectory / L"DuplicateTwo", "test.duplicate", "Duplicate Two", 420.0F);
    SC13_EXPECT(manager.Refresh(error));
    entries = manager.Snapshot();
    const auto duplicateFailures = std::count_if(
        entries.begin(), entries.end(), [](const sc13::mods::ModUiEntry& entry) {
            return entry.id == "test.duplicate" &&
                   entry.state == sc13::mods::ModState::Failed &&
                   entry.diagnostic.find("DuplicateOne") != std::string::npos &&
                   entry.diagnostic.find("DuplicateTwo") != std::string::npos;
        });
    SC13_EXPECT(duplicateFailures == 2);

    std::vector<sc13::mods::ModId> persisted;
    SC13_EXPECT(sc13::mods::LoadEnabledState(statePath, persisted, error));
    SC13_EXPECT(persisted.empty());
    std::filesystem::remove_all(root, directoryError);
    SC13_EXPECT(!directoryError);
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
    TestToon();
    TestModDefinitionParser();
    TestPatchRegistry();
    TestRuntimeResourceCache();
    TestLoaderConfig();
    TestLogEventClassification();
    TestModStateStore();
    TestModManager();
    TestDbpf();
    TestPeFile();
    if (g_failures == 0) {
        std::cout << "All SC13 tests passed.\n";
        return 0;
    }
    std::cerr << g_failures << " test expectation(s) failed.\n";
    return 1;
}
