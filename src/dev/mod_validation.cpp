#include "dev/mod_validation.hpp"
#include "core/json.hpp"
#include "core/sha256.hpp"
#include "core/runtime_property_table.hpp"
#include "formats/dbpf/dbpf_reader.hpp"
#include "formats/prop/prop_document.hpp"
#include "mods/patch_registry.hpp"
#include <algorithm>
#include <bit>
#include <set>
#include <sstream>

namespace sc13::dev {
ValidationReport ValidateMods(std::span<const mods::ModDefinition> definitions,
    const std::filesystem::path& gameDirectory) {
    ValidationReport report;
    report.checkedGame = !gameDirectory.empty();
    mods::PatchRegistry registry;
    std::set<core::Tgi> targets;
    for (const auto& definition : definitions) {
        std::set<core::Tgi> affected;
        std::string error;
        if (!registry.Activate(definition.manifest.id, definition.patches, affected, error)) {
            report.errors.push_back(error);
            report.valid = false;
        }
        targets.insert(affected.begin(), affected.end());
    }
    const auto snapshot = registry.Current();
    report.conflicts = snapshot->conflicts();
    std::vector<formats::dbpf::DbpfPackage> packages;
    if (report.checkedGame) {
        std::error_code scanError;
        std::vector<std::filesystem::path> paths;
        for (std::filesystem::recursive_directory_iterator it(gameDirectory, scanError), end;
            !scanError && it != end; it.increment(scanError)) {
            if (it->is_regular_file(scanError) && it->path().extension() == L".package") paths.push_back(it->path());
        }
        if (scanError || paths.empty()) {
            report.errors.push_back(scanError ? "Package scan failed: " + scanError.message() : "No .package files found");
            report.valid = false;
        }
        std::sort(paths.begin(), paths.end());
        for (const auto& path : paths) {
            formats::dbpf::DbpfPackage package;
            std::string error;
            if (!package.Open(path, error)) {
                report.errors.push_back(core::Utf8Text(path.u8string()) + ": " + error);
                report.valid = false;
            } else packages.push_back(std::move(package));
        }
    }
    for (const auto& target : targets) {
        ResourceValidation resource;
        resource.target = target;
        std::vector<std::pair<std::size_t, std::size_t>> matches;
        for (std::size_t packageIndex = 0; packageIndex < packages.size(); ++packageIndex) {
            const auto& package = packages[packageIndex];
            const auto entries = package.entries();
            for (std::size_t index = 0; index < entries.size(); ++index) {
                if (entries[index].tgi == target && entries[index].instanceHigh == 0U) {
                    resource.packages.push_back(core::Utf8Text(package.path().u8string()));
                    matches.emplace_back(packageIndex, index);
                }
            }
        }
        formats::prop::PropDocument document;
        bool valuesAvailable = false;
        if (report.checkedGame) {
            if (matches.empty()) resource.error = "Target resource not found";
            else if (matches.size() != 1U) resource.error = "Ambiguous TGI: select a directory containing the intended source package only; load precedence is unverified";
            else {
                const auto [packageIndex, entryIndex] = matches.front();
                std::vector<std::byte> bytes;
                if (packages[packageIndex].ReadResource(packages[packageIndex].entries()[entryIndex], bytes, resource.error)) {
                    core::Sha256Digest digest;
                    if (core::Sha256(bytes, digest, resource.error)) {
                        resource.resourceSha256 = core::ToHex(digest);
                        valuesAvailable = document.Parse(bytes, resource.error);
                    }
                }
            }
        }
        std::vector<core::RuntimePropertyRecord> current;
        if (valuesAvailable) {
            for (const auto& property : document.properties()) {
                if (const auto value = document.GetFloat(property.identifier))
                    current.push_back(core::RuntimePropertyRecord{property.identifier, std::bit_cast<std::uint32_t>(*value), {}, 0x000D0000U});
            }
            std::sort(current.begin(), current.end(), [](const auto& left, const auto& right) { return left.id < right.id; });
        }
        for (const auto& registered : *snapshot->Find(target)) {
            for (const auto& property : registered.patch.properties) {
                ValidationStep step{registered.owner, target, property};
                if (valuesAvailable) {
                    const auto before = document.GetFloat(property.propertyId);
                    if (!before) {
                        resource.error = "Property '" + property.name + "' id=" + std::to_string(property.propertyId) + " missing or not scalar float";
                    } else {
                        const auto currentValue = core::ReadRuntimeFloat(current, property.propertyId);
                        mods::RegisteredPatch single = registered;
                        single.patch.properties = {property};
                        std::vector<core::RuntimePropertyRecord> result;
                        std::string error;
                        if (currentValue && mods::ApplyPatchSequence(target, current, std::span(&single, 1), result, error)) {
                            step.before = *currentValue;
                            step.after = *core::ReadRuntimeFloat(result, property.propertyId);
                            step.hasValues = true;
                            current = std::move(result);
                        } else resource.error = error.empty() ? "Invalid property table" : error;
                    }
                }
                report.steps.push_back(std::move(step));
            }
        }
        if (!resource.error.empty()) report.valid = false;
        report.resources.push_back(std::move(resource));
    }
    return report;
}

std::string ValidationJson(const ValidationReport& report) {
    using core::JsonString;
    std::string json = "{\"schemaVersion\":1,\"valid\":" + std::string(report.valid ? "true" : "false") +
        ",\"checkedGame\":" + (report.checkedGame ? "true" : "false") + ",\"errors\":[";
    bool comma = false;
    for (const auto& error : report.errors) { if (comma) json += ','; comma = true; json += JsonString(error); }
    json += "],\"resources\":["; comma = false;
    for (const auto& resource : report.resources) {
        if (comma) json += ','; comma = true;
        json += "{\"tgi\":" + JsonString(core::ToString(resource.target)) + ",\"error\":" + JsonString(resource.error) +
            ",\"resourceSha256\":" + JsonString(resource.resourceSha256) + ",\"packages\":[";
        bool pathComma = false;
        for (const auto& path : resource.packages) { if (pathComma) json += ','; pathComma = true; json += JsonString(path); }
        json += "]}";
    }
    json += "],\"steps\":["; comma = false;
    for (const auto& step : report.steps) {
        if (comma) json += ','; comma = true;
        json += "{\"modId\":" + JsonString(step.owner) + ",\"tgi\":" + JsonString(core::ToString(step.target)) +
            ",\"propertyId\":" + std::to_string(step.property.propertyId) + ",\"name\":" + JsonString(step.property.name) +
            ",\"source\":" + JsonString(step.property.source) + ",\"operation\":" + JsonString(mods::PatchOperationName(step.property.operation)) +
            ",\"description\":" + JsonString(step.property.description) + ",\"unit\":" + JsonString(step.property.unit) + ",\"origin\":" + JsonString(step.property.origin) +
            ",\"operand\":" + core::JsonFloat(step.property.value) + ",\"before\":" + (step.hasValues ? core::JsonFloat(step.before) : "null") +
            ",\"after\":" + (step.hasValues ? core::JsonFloat(step.after) : "null") + "}";
    }
    json += "],\"conflicts\":["; comma = false;
    for (const auto& conflict : report.conflicts) {
        if (comma) json += ','; comma = true;
        json += "{\"tgi\":" + JsonString(core::ToString(conflict.target)) + ",\"propertyId\":" + std::to_string(conflict.propertyId) +
            ",\"earlier\":" + JsonString(conflict.earlierOwner) + ",\"later\":" + JsonString(conflict.laterOwner) + "}";
    }
    return json + "]}\n";
}

std::string ValidationText(const ValidationReport& report) {
    std::ostringstream output;
    output << (report.valid ? "VALID" : "INVALID") << (report.checkedGame ? " (package data checked)\n" : " (definitions only; game values unchecked)\n");
    for (const auto& error : report.errors) output << "ERROR " << error << '\n';
    for (const auto& resource : report.resources) {
        output << "TGI " << core::ToString(resource.target) << '\n';
        for (const auto& path : resource.packages) output << "  source " << path << '\n';
        if (!resource.error.empty()) output << "  ERROR " << resource.error << '\n';
    }
    for (const auto& step : report.steps) {
        output << step.owner << " " << core::ToString(step.target) << " " << step.property.name << " [0x" << std::hex << step.property.propertyId << std::dec << "] ";
        if (step.hasValues) output << core::JsonFloat(step.before) << " -> ";
        output << mods::PatchOperationName(step.property.operation) << ' ' << core::JsonFloat(step.property.value);
        if (step.hasValues) output << " -> " << core::JsonFloat(step.after);
        output << " source=" << step.property.source << '\n';
    }
    for (const auto& conflict : report.conflicts) output << "CONFLICT " << conflict.earlierOwner << " -> " << conflict.laterOwner << " TGI=" << core::ToString(conflict.target) << " property=" << conflict.propertyId << '\n';
    return output.str();
}
}
