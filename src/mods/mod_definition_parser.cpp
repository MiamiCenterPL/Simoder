#include "mods/mod_definition_parser.hpp"

#include "formats/toon/toon_document.hpp"
#include "core/json.hpp"

#include <charconv>
#include <cmath>
#include <cstdio>
#include <limits>
#include <map>
#include <set>
#include <string_view>

namespace sc13::mods {
namespace {

using ToonObject = formats::toon::Value::Object;

/** @summary Stores per-document names resolved before runtime patch registration. */
struct NameCatalog final {
    std::map<std::string, std::uint32_t, std::less<>> properties;
    std::map<std::string, core::Tgi, std::less<>> resources;
    std::map<std::string, std::string, std::less<>> propertySources;
    std::map<std::string, std::string, std::less<>> resourceSources;
    std::map<std::string, PropertyPatch, std::less<>> metadata;
    std::map<std::string, std::string, std::less<>> declaredTypes;
};

/** Formats a bounded TOON parser diagnostic for one named mod file. */
[[nodiscard]] std::string FormatParseError(
    const std::filesystem::path& path,
    const formats::toon::ParseError& error) {
    std::string message = path.filename().string();
    if (error.line != 0U) {
        message += ":" + std::to_string(error.line);
        if (error.column != 0U) {
            message += ":" + std::to_string(error.column);
        }
    }
    message += ": " + error.message;
    return message;
}

/** Finds one root object member. */
[[nodiscard]] const formats::toon::Value* Find(
    const ToonObject& object,
    std::string_view key) noexcept {
    const auto iterator = object.find(key);
    return iterator == object.end() ? nullptr : &iterator->second;
}

/** Reads one required non-empty bounded string field. */
[[nodiscard]] bool ReadRequiredString(
    const ToonObject& object,
    std::string_view key,
    std::size_t maximumLength,
    std::string& value,
    std::string& error) {
    const formats::toon::Value* const field = Find(object, key);
    const std::string* const text = field == nullptr ? nullptr : field->AsString();
    if (text == nullptr || text->empty()) {
        error = "Required string field '" + std::string(key) + "' is missing or empty";
        return false;
    }
    if (text->size() > maximumLength) {
        error = "String field '" + std::string(key) + "' exceeds its length limit";
        return false;
    }
    value = *text;
    return true;
}

/** Reads one optional bounded string field. */
[[nodiscard]] bool ReadOptionalString(
    const ToonObject& object,
    std::string_view key,
    std::size_t maximumLength,
    std::string& value,
    std::string& error) {
    const formats::toon::Value* const field = Find(object, key);
    if (field == nullptr) {
        value.clear();
        return true;
    }
    const std::string* const text = field->AsString();
    if (text == nullptr || text->size() > maximumLength) {
        error = "Optional field '" + std::string(key) + "' must be a bounded string";
        return false;
    }
    value = *text;
    return true;
}

/** Parses a uint32 from a decimal number or 0x-prefixed string. */
[[nodiscard]] bool ReadUint32(
    const ToonObject& object,
    std::string_view key,
    std::uint32_t& value,
    std::string& error) {
    const formats::toon::Value* const field = Find(object, key);
    if (field == nullptr) {
        error = "Required uint32 field '" + std::string(key) + "' is missing";
        return false;
    }
    if (const double* number = field->AsNumber();
        number != nullptr && *number >= 0.0 &&
        *number <= static_cast<double>(std::numeric_limits<std::uint32_t>::max()) &&
        std::floor(*number) == *number) {
        value = static_cast<std::uint32_t>(*number);
        return true;
    }
    const std::string* const text = field->AsString();
    if (text == nullptr || text->size() <= 2U ||
        ((*text)[0] != '0' || ((*text)[1] != 'x' && (*text)[1] != 'X'))) {
        error = "Field '" + std::string(key) + "' must be uint32 or a 0x-prefixed string";
        return false;
    }
    std::uint32_t parsed = 0U;
    const char* const begin = text->data() + 2U;
    const char* const end = text->data() + text->size();
    const auto result = std::from_chars(begin, end, parsed, 16);
    if (result.ec != std::errc{} || result.ptr != end) {
        error = "Field '" + std::string(key) + "' contains an invalid hexadecimal uint32";
        return false;
    }
    value = parsed;
    return true;
}

/** Parses the currently supported typed property kind. */
[[nodiscard]] bool ReadPropertyType(
    const ToonObject& object,
    PropertyType& type,
    std::string& error) {
    std::string text;
    if (!ReadRequiredString(object, "type", 32U, text, error)) {
        return false;
    }
    if (text != "float") {
        error = "Unsupported property type '" + text + "'; M3 supports only float";
        return false;
    }
    type = PropertyType::Float;
    return true;
}

/** Parses a deterministic arithmetic operation. */
[[nodiscard]] bool ReadOperation(
    const ToonObject& object,
    PatchOperation& operation,
    std::string& error) {
    std::string text;
    if (!ReadRequiredString(object, "operation", 32U, text, error)) {
        return false;
    }
    if (text == "set") {
        operation = PatchOperation::Set;
    } else if (text == "add") {
        operation = PatchOperation::Add;
    } else if (text == "subtract") {
        operation = PatchOperation::Subtract;
    } else if (text == "multiply") {
        operation = PatchOperation::Multiply;
    } else if (text == "divide") {
        operation = PatchOperation::Divide;
    } else {
        error = "Unsupported patch operation '" + text + "'";
        return false;
    }
    return true;
}

/** Parses one finite float operand and rejects unsafe division. */
[[nodiscard]] bool ReadPatchValue(
    const ToonObject& object,
    PatchOperation operation,
    float& value,
    std::string& error) {
    const formats::toon::Value* const field = Find(object, "value");
    const double* const number = field == nullptr ? nullptr : field->AsNumber();
    if (number == nullptr || !std::isfinite(*number) ||
        *number < -static_cast<double>(std::numeric_limits<float>::max()) ||
        *number > static_cast<double>(std::numeric_limits<float>::max())) {
        error = "Patch field 'value' must be a finite float";
        return false;
    }
    value = static_cast<float>(*number);
    if (operation == PatchOperation::Divide && value == 0.0F) {
        error = "Divide patch cannot use zero";
        return false;
    }
    return true;
}

/** Maps one parsed manifest into its validated typed model. */
[[nodiscard]] bool ParseManifest(
    const formats::toon::Document& document,
    ModManifest& manifest,
    std::string& error) {
    ModManifest parsed;
    if (!ReadRequiredString(document.root(), "id", 128U, parsed.id, error) ||
        !ReadRequiredString(document.root(), "name", 128U, parsed.name, error) ||
        !ReadRequiredString(document.root(), "version", 64U, parsed.version, error) ||
        !ReadOptionalString(document.root(), "author", 128U, parsed.author, error) ||
        !ReadOptionalString(
            document.root(), "description", 1024U, parsed.description, error)) {
        return false;
    }
    if (!IsValidModId(parsed.id)) {
        error = "Manifest id '" + parsed.id + "' is not a valid stable mod identity";
        return false;
    }
    manifest = std::move(parsed);
    return true;
}

/** @summary Resolves an optional name and verifies any accompanying numeric identity. */
[[nodiscard]] bool ResolveProperty(
    const ToonObject& object, const NameCatalog& catalog,
    std::uint32_t& id, std::string& error) {
    if (Find(object, "name") == nullptr) {
        return ReadUint32(object, "id", id, error);
    }
    std::string name;
    if (!ReadRequiredString(object, "name", 128U, name, error)) return false;
    const auto match = catalog.properties.find(name);
    if (match == catalog.properties.end()) {
        error = "Unknown property name '" + name + "'";
        return false;
    }
    if (Find(object, "id") != nullptr &&
        (!ReadUint32(object, "id", id, error) || id != match->second)) {
        if (error.empty()) error = "Property name/id mismatch for '" + name + "'";
        return false;
    }
    id = match->second;
    return true;
}

/** @summary Reads an exact TGI without hashing or inferring resource names. */
[[nodiscard]] bool ReadTarget(const ToonObject& object, core::Tgi& target, std::string& error) {
    return ReadUint32(object, "type", target.type, error) &&
        ReadUint32(object, "group", target.group, error) &&
        ReadUint32(object, "instance", target.instance, error);
}

/** @summary Loads explicit resource/property aliases with mandatory provenance. */
[[nodiscard]] bool ParseNames(const ToonObject& root, NameCatalog& catalog, std::string& error) {
    const auto* value = Find(root, "symbols");
    if (value == nullptr) return true;
    const auto* symbols = value->AsObject();
    if (symbols == nullptr) { error = "symbols must be an object"; return false; }
    for (const std::string_view kind : {"properties", "resources"}) {
        const auto* entries = Find(*symbols, kind);
        if (entries == nullptr) continue;
        const auto* array = entries->AsArray();
        if (array == nullptr) { error = "symbols entries must be arrays"; return false; }
        for (const auto& entry : *array) {
            const auto* object = entry.AsObject();
            std::string name, source;
            if (object == nullptr) { error = "symbol must be an object"; return false; }
            if (!ReadRequiredString(*object, "name", 128U, name, error) ||
                !ReadRequiredString(*object, "source", 1024U, source, error)) return false;
            bool inserted = false;
            if (kind == "properties") {
                std::uint32_t id{};
                if (!ReadUint32(*object, "id", id, error)) return false;
                inserted = catalog.properties.emplace(name, id).second;
                catalog.propertySources.emplace(name, source);
                PropertyPatch metadata;
                std::string declaredType;
                if (!ReadOptionalString(*object, "description", 4096U, metadata.description, error) ||
                    !ReadOptionalString(*object, "unit", 64U, metadata.unit, error) ||
                    !ReadOptionalString(*object, "origin", 64U, metadata.origin, error) ||
                    !ReadOptionalString(*object, "type", 64U, declaredType, error)) return false;
                catalog.metadata.emplace(name, std::move(metadata));
                catalog.declaredTypes.emplace(name, std::move(declaredType));
            } else {
                core::Tgi target{};
                if (!ReadTarget(*object, target, error)) return false;
                inserted = catalog.resources.emplace(name, target).second;
                catalog.resourceSources.emplace(name, source);
            }
            if (!inserted) { error = "Duplicate symbol name '" + name + "'"; return false; }
        }
    }
    return true;
}

/** @summary Maps a named or numeric property into a typed operation. */
[[nodiscard]] bool ParsePropertyPatch(
    const ToonObject& object,
    const NameCatalog& catalog,
    PropertyPatch& patch,
    std::string& error) {
    PropertyPatch parsed;
    if (!ResolveProperty(object, catalog, parsed.propertyId, error) ||
        !ReadPropertyType(object, parsed.type, error) ||
        !ReadOperation(object, parsed.operation, error) ||
        !ReadPatchValue(object, parsed.operation, parsed.value, error)) {
        return false;
    }
    if (Find(object, "name") != nullptr) {
        if (!ReadRequiredString(object, "name", 128U, parsed.name, error)) return false;
        parsed.source = catalog.propertySources.at(parsed.name);
        const auto& metadata = catalog.metadata.at(parsed.name);
        parsed.description = metadata.description; parsed.unit = metadata.unit; parsed.origin = metadata.origin;
        const auto& declaredType = catalog.declaredTypes.at(parsed.name);
        if (!declaredType.empty() && declaredType != "float") {
            error = "Symbol '" + parsed.name + "' declares type '" + declaredType + "', incompatible with float patch";
            return false;
        }
    }
    patch = std::move(parsed);
    return true;
}

/** Maps one target and its property array into a resource patch definition. */
[[nodiscard]] bool ParseResourcePatch(
    const ToonObject& object,
    const NameCatalog& catalog,
    ResourcePatchDefinition& patch,
    std::string& error) {
    const formats::toon::Value* const targetValue = Find(object, "target");
    const ToonObject* const target =
        targetValue == nullptr ? nullptr : targetValue->AsObject();
    if (target == nullptr) {
        error = "Patch target must be an object";
        return false;
    }
    ResourcePatchDefinition parsed;
    if (Find(*target, "name") != nullptr) {
        std::string name;
        if (!ReadRequiredString(*target, "name", 128U, name, error)) return false;
        const auto match = catalog.resources.find(name);
        if (match == catalog.resources.end()) {
            error = "Unknown resource name '" + name + "'";
            return false;
        }
        parsed.target = match->second;
        parsed.name = name;
        parsed.source = catalog.resourceSources.at(name);
        if (Find(*target, "type") || Find(*target, "group") || Find(*target, "instance")) {
            core::Tgi explicitTarget{};
            if (!ReadTarget(*target, explicitTarget, error)) return false;
            if (!(explicitTarget == parsed.target)) {
                error = "Resource name/TGI mismatch for '" + name + "'";
                return false;
            }
        }
    } else if (!ReadTarget(*target, parsed.target, error)) {
        error = "Patch target: " + error;
        return false;
    }

    const formats::toon::Value* const propertiesValue = Find(object, "properties");
    const formats::toon::Value::Array* const properties =
        propertiesValue == nullptr ? nullptr : propertiesValue->AsArray();
    if (properties == nullptr || properties->empty()) {
        error = "Patch properties must be a non-empty array";
        return false;
    }
    std::set<std::uint32_t> identifiers;
    for (const formats::toon::Value& value : *properties) {
        const ToonObject* const propertyObject = value.AsObject();
        if (propertyObject == nullptr) {
            error = "Every property patch must be an object";
            return false;
        }
        PropertyPatch property;
        if (!ParsePropertyPatch(*propertyObject, catalog, property, error)) {
            return false;
        }
        if (!identifiers.insert(property.propertyId).second) {
            error = "One mod cannot patch the same property twice in a resource block";
            return false;
        }
        parsed.properties.push_back(property);
    }
    patch = std::move(parsed);
    return true;
}

/** Maps and validates the root patch list. */
[[nodiscard]] bool ParseOverrides(
    const formats::toon::Document& document,
    NameCatalog catalog,
    std::vector<ResourcePatchDefinition>& patches,
    std::string& error) {
    const formats::toon::Value* const patchesValue = Find(document.root(), "patches");
    const formats::toon::Value::Array* const patchArray =
        patchesValue == nullptr ? nullptr : patchesValue->AsArray();
    if (patchArray == nullptr || patchArray->empty()) {
        error = "overrides.toon requires a non-empty patches array";
        return false;
    }
    std::set<std::pair<core::Tgi, std::uint32_t>> uniqueProperties;
    if (!ParseNames(document.root(), catalog, error)) return false;
    std::vector<ResourcePatchDefinition> parsed;
    for (const formats::toon::Value& value : *patchArray) {
        const ToonObject* const object = value.AsObject();
        if (object == nullptr) {
            error = "Every resource patch must be an object";
            return false;
        }
        ResourcePatchDefinition patch;
        if (!ParseResourcePatch(*object, catalog, patch, error)) {
            return false;
        }
        for (const PropertyPatch& property : patch.properties) {
            if (!uniqueProperties.emplace(patch.target, property.propertyId).second) {
                error = "One mod cannot patch the same TGI/property more than once";
                return false;
            }
        }
        parsed.push_back(std::move(patch));
    }
    patches = std::move(parsed);
    return true;
}

}  // namespace

bool IsValidModId(std::string_view id) noexcept {
    if (id.empty() || id.size() > 128U || id.front() == '.' || id.back() == '.') {
        return false;
    }
    bool previousSeparator = false;
    for (const char character : id) {
        const bool alphanumeric =
            (character >= 'a' && character <= 'z') ||
            (character >= '0' && character <= '9');
        const bool separator = character == '.' || character == '-' || character == '_';
        if (!alphanumeric && !separator) {
            return false;
        }
        if (separator && previousSeparator) {
            return false;
        }
        previousSeparator = separator;
    }
    return !previousSeparator;
}

bool LoadModDefinition(
    const std::filesystem::path& directory,
    ModDefinition& definition,
    std::string& error) noexcept {
    try {
        definition = {};
        error.clear();
        formats::toon::Document manifestDocument;
        formats::toon::ParseError parseError;
        const std::filesystem::path manifestPath = directory / L"mod.toon";
        if (!formats::toon::LoadFile(manifestPath, manifestDocument, parseError)) {
            error = FormatParseError(manifestPath, parseError);
            return false;
        }
        ModManifest manifest;
        if (!ParseManifest(manifestDocument, manifest, error)) {
            error = "mod.toon: " + error;
            return false;
        }

        formats::toon::Document overridesDocument;
        const std::filesystem::path overridesPath = directory / L"overrides.toon";
        if (!formats::toon::LoadFile(overridesPath, overridesDocument, parseError)) {
            error = FormatParseError(overridesPath, parseError);
            return false;
        }
        std::vector<ResourcePatchDefinition> patches;
        NameCatalog catalog;
        const auto symbolsPath = directory / L"symbols.toon";
        std::error_code statusError;
        const bool hasSymbols = std::filesystem::exists(symbolsPath, statusError);
        if (statusError) { error = "symbols.toon: " + statusError.message(); return false; }
        if (hasSymbols) {
            formats::toon::Document symbolsDocument;
            if (!formats::toon::LoadFile(symbolsPath, symbolsDocument, parseError)) {
                error = FormatParseError(symbolsPath, parseError);
                return false;
            }
            if (Find(symbolsDocument.root(), "symbols") == nullptr) {
                error = "symbols.toon requires a symbols object";
                return false;
            }
            if (!ParseNames(symbolsDocument.root(), catalog, error)) return false;
        }
        if (!ParseOverrides(overridesDocument, std::move(catalog), patches, error)) {
            error = "overrides.toon: " + error;
            return false;
        }

        definition =
            ModDefinition{std::move(manifest), directory, std::move(patches)};
        return true;
    } catch (...) {
        definition = {};
        error = "Mod definition load failed due to an allocation exception";
        return false;
    }
}

bool ListSymbolsJson(const std::filesystem::path& directory, std::string_view query,
    std::string& json, std::string& error) noexcept {
    try {
        error.clear(); json.clear(); NameCatalog catalog;
        for (const auto* filename : {L"symbols.toon", L"overrides.toon"}) {
            std::error_code statusError;
            const auto path = directory / filename;
            const bool exists = std::filesystem::exists(path, statusError);
            if (statusError) { error = statusError.message(); return false; }
            if (!exists) continue;
            formats::toon::Document document;
            formats::toon::ParseError parseError;
            if (!formats::toon::LoadFile(path, document, parseError)) { error = FormatParseError(path, parseError); return false; }
            if (!ParseNames(document.root(), catalog, error)) return false;
        }
        json = "{\"schemaVersion\":1,\"symbols\":[";
        bool comma = false;
        for (const auto& [name, id] : catalog.properties) {
            char identifier[11]{};
            std::snprintf(identifier, sizeof(identifier), "0x%08X", id);
            if (!query.empty() && name.find(query) == std::string::npos && std::string_view(identifier).find(query) == std::string::npos) continue;
            if (comma) json += ','; comma = true;
            const auto& metadata = catalog.metadata.at(name);
            json += "{\"kind\":\"property\",\"name\":" + core::JsonString(name) + ",\"id\":" + core::JsonString(identifier) +
                ",\"type\":" + core::JsonString(catalog.declaredTypes.at(name)) + ",\"source\":" + core::JsonString(catalog.propertySources.at(name)) +
                ",\"description\":" + core::JsonString(metadata.description) + ",\"unit\":" + core::JsonString(metadata.unit) +
                ",\"origin\":" + core::JsonString(metadata.origin) + "}";
        }
        for (const auto& [name, target] : catalog.resources) {
            const auto tgi = core::ToString(target);
            if (!query.empty() && name.find(query) == std::string::npos && tgi.find(query) == std::string::npos) continue;
            if (comma) json += ','; comma = true;
            json += "{\"kind\":\"resource\",\"name\":" + core::JsonString(name) + ",\"tgi\":" + core::JsonString(tgi) +
                ",\"source\":" + core::JsonString(catalog.resourceSources.at(name)) + "}";
        }
        json += "]}\n";
        return true;
    } catch (...) { json.clear(); error = "Could not read catalog"; return false; }
}
}  // namespace sc13::mods
