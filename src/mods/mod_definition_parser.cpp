#include "mods/mod_definition_parser.hpp"

#include "formats/toon/toon_document.hpp"

#include <charconv>
#include <cmath>
#include <limits>
#include <set>
#include <string_view>

namespace sc13::mods {
namespace {

using ToonObject = formats::toon::Value::Object;

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

/** Maps one validated property object into a typed operation. */
[[nodiscard]] bool ParsePropertyPatch(
    const ToonObject& object,
    PropertyPatch& patch,
    std::string& error) {
    PropertyPatch parsed;
    if (!ReadUint32(object, "id", parsed.propertyId, error) ||
        !ReadPropertyType(object, parsed.type, error) ||
        !ReadOperation(object, parsed.operation, error) ||
        !ReadPatchValue(object, parsed.operation, parsed.value, error)) {
        return false;
    }
    patch = parsed;
    return true;
}

/** Maps one target and its property array into a resource patch definition. */
[[nodiscard]] bool ParseResourcePatch(
    const ToonObject& object,
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
    if (!ReadUint32(*target, "type", parsed.target.type, error) ||
        !ReadUint32(*target, "group", parsed.target.group, error) ||
        !ReadUint32(*target, "instance", parsed.target.instance, error)) {
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
        if (!ParsePropertyPatch(*propertyObject, property, error)) {
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
    std::vector<ResourcePatchDefinition> parsed;
    for (const formats::toon::Value& value : *patchArray) {
        const ToonObject* const object = value.AsObject();
        if (object == nullptr) {
            error = "Every resource patch must be an object";
            return false;
        }
        ResourcePatchDefinition patch;
        if (!ParseResourcePatch(*object, patch, error)) {
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
        if (!ParseOverrides(overridesDocument, patches, error)) {
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

}  // namespace sc13::mods
