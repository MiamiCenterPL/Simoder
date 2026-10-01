#include "dev/runtime_diagnostics.hpp"
#include "core/json.hpp"
#include "runtime/runtime_resource_cache.hpp"
#include <Windows.h>
#include <chrono>
#include <fstream>

namespace sc13::dev {
namespace {
/** @summary Serializes resolved operations and optional pending definitions with bounded provenance. */
std::string PatchesJson(std::span<const mods::ResourcePatchDefinition> patches) {
    std::string json = "["; bool comma = false;
    for (const auto& patch : patches) {
        if (comma) json += ','; comma = true;
        json += "{\"tgi\":" + core::JsonString(core::ToString(patch.target)) + ",\"name\":" + core::JsonString(patch.name) +
            ",\"source\":" + core::JsonString(patch.source) + ",\"properties\":[";
        bool propertyComma = false;
        for (const auto& property : patch.properties) {
            if (propertyComma) json += ','; propertyComma = true;
            json += "{\"id\":" + std::to_string(property.propertyId) + ",\"name\":" + core::JsonString(property.name) +
                ",\"source\":" + core::JsonString(property.source) + ",\"operation\":" + core::JsonString(mods::PatchOperationName(property.operation)) +
                ",\"operand\":" + core::JsonFloat(property.value) + ",\"description\":" + core::JsonString(property.description) +
                ",\"unit\":" + core::JsonString(property.unit) + ",\"origin\":" + core::JsonString(property.origin) + "}";
        }
        json += "]}";
    }
    return json + ']';
}
}

std::string RuntimeDiagnosticsJson(const mods::ModManager& manager, std::uint32_t processId) {
    const auto generation = manager.Generation();
    const auto mods = manager.Snapshot();
    const auto observations = manager.RuntimeObservations();
    const auto conflicts = manager.Conflicts();
    const auto generated = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
    std::string json = "{\"schemaVersion\":1,\"pid\":" + std::to_string(processId) + ",\"generatedAtUnixMs\":" + std::to_string(generated) +
        ",\"generation\":" + std::to_string(generation) + ",\"consistentGeneration\":" + (generation == manager.Generation() ? "true" : "false") +
        ",\"evidence\":\"historical committed resource values; copied consumers and gameplay effects unverified\",\"mods\":[";
    bool comma = false;
    for (const auto& mod : mods) {
        if (comma) json += ','; comma = true;
        json += "{\"id\":" + core::JsonString(mod.id) + ",\"name\":" + core::JsonString(mod.name) + ",\"state\":" + core::JsonString(mods::ModStateName(mod.state)) +
            ",\"diagnostic\":" + core::JsonString(mod.diagnostic) + ",\"canReload\":" + (mod.canReload ? "true" : "false") +
            ",\"pendingRevision\":" + core::JsonString(mod.pendingRevision) +
            ",\"patches\":" + PatchesJson(mod.patches) + ",\"pendingPatches\":" + PatchesJson(mod.pendingPatches) + "}";
    }
    json += "],\"resources\":["; comma = false;
    for (const auto& observation : observations) {
        if (comma) json += ','; comma = true;
        json += "{\"tgi\":" + core::JsonString(core::ToString(observation.key.target)) + ",\"instanceToken\":" + std::to_string(observation.key.resourceAddress) +
            ",\"generation\":" + std::to_string(observation.generation) + ",\"status\":" + core::JsonString(observation.status) +
            ",\"lastCommittedGeneration\":" + std::to_string(observation.committedGeneration) +
            ",\"diagnostic\":" + core::JsonString(observation.diagnostic) + ",\"observedAtUnixMs\":" + std::to_string(observation.observedAtUnixMs) + ",\"values\":[";
        bool valueComma = false;
        std::set<std::uint32_t> selected;
        for (const auto& mod : mods) for (const auto& patch : mod.patches)
            if (patch.target == observation.key.target) for (const auto& property : patch.properties) selected.insert(property.propertyId);
        for (const auto id : selected) {
            if (valueComma) json += ','; valueComma = true;
            const auto vanilla = core::ReadRuntimeFloat(observation.vanilla, id);
            const auto applied = core::ReadRuntimeFloat(observation.applied, id);
            json += "{\"id\":" + std::to_string(id) + ",\"vanilla\":" + (vanilla ? core::JsonFloat(*vanilla) : "null") +
                ",\"lastCommitted\":" + (applied ? core::JsonFloat(*applied) : "null") + "}";
        }
        json += "]}";
    }
    json += "],\"conflicts\":["; comma = false;
    for (const auto& conflict : conflicts) {
        if (comma) json += ','; comma = true;
        json += "{\"tgi\":" + core::JsonString(core::ToString(conflict.target)) + ",\"propertyId\":" + std::to_string(conflict.propertyId) +
            ",\"earlier\":" + core::JsonString(conflict.earlierOwner) + ",\"later\":" + core::JsonString(conflict.laterOwner) + "}";
    }
    return json + "]}\n";
}

RuntimeDiagnostics::RuntimeDiagnostics(mods::ModManager& manager, std::filesystem::path destination)
    : worker_([this, &manager, destination = std::move(destination)](std::stop_token stop) {
        while (!stop.stop_requested()) {
            try {
                std::string refreshError;
                static_cast<void>(manager.Refresh(refreshError));
                const auto json = RuntimeDiagnosticsJson(manager, GetCurrentProcessId());
                if (json.size() <= 1024U * 1024U) {
                    auto temporary = destination; temporary += L".tmp";
                    std::ofstream file(temporary, std::ios::binary | std::ios::trunc);
                    file << json; file.close();
                    if (file) static_cast<void>(MoveFileExW(temporary.c_str(), destination.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH));
                }
            } catch (...) { /* @summary Optional developer publishing cannot stop runtime patches. */ }
            std::unique_lock lock(waitMutex_);
            wake_.wait_for(lock, std::chrono::seconds(1), [&stop] { return stop.stop_requested(); });
        }
        std::error_code error;
        std::filesystem::remove(destination, error);
    }) {}

RuntimeDiagnostics::~RuntimeDiagnostics() {
    worker_.request_stop(); wake_.notify_all();
    if (worker_.joinable()) worker_.join();
}
}
