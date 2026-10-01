#include "mods/mod_manager.hpp"

#include "mods/mod_definition_parser.hpp"
#include "mods/mod_state_store.hpp"
#include "mods/patch_registry.hpp"
#include "runtime/runtime_resource_cache.hpp"

#include <Windows.h>

#include <algorithm>
#include <set>
#include <system_error>
#include <utility>

namespace sc13::mods {
namespace {

/** Appends a diagnostic without discarding earlier independent failures. */
void AppendError(std::string& destination, std::string_view message) {
    if (!destination.empty()) {
        destination += "; ";
    }
    destination += message;
}

/** Returns a stable narrow display form for one filesystem path. */
[[nodiscard]] std::string DisplayPath(const std::filesystem::path& path) {
    try {
        return path.filename().string();
    } catch (...) {
        return "<unrepresentable-path>";
    }
}

/** Hashes both declarative inputs used to construct a mod definition. */
[[nodiscard]] bool HashDefinitionFiles(
    const std::filesystem::path& directory,
    ModDefinitionFingerprint& fingerprint,
    std::string& error) noexcept {
    if (!core::Sha256File(directory / L"mod.toon", fingerprint.manifest, error)) {
        error = "mod.toon fingerprint: " + error;
        return false;
    }
    if (!core::Sha256File(directory / L"overrides.toon", fingerprint.overrides, error)) {
        error = "overrides.toon fingerprint: " + error;
        return false;
    }
    return true;
}

/** Loads a definition only when its source bytes remain stable throughout parsing. */
[[nodiscard]] bool LoadStableCandidate(
    const std::filesystem::path& directory,
    ModDefinition& definition,
    ModDefinitionFingerprint& fingerprint,
    std::string& error) noexcept {
    for (unsigned int attempt = 0U; attempt < 2U; ++attempt) {
        ModDefinitionFingerprint before;
        if (!HashDefinitionFiles(directory, before, error)) {
            return false;
        }
        ModDefinition parsed;
        if (!LoadModDefinition(directory, parsed, error)) {
            return false;
        }
        ModDefinitionFingerprint after;
        if (!HashDefinitionFiles(directory, after, error)) {
            return false;
        }
        if (before == after) {
            definition = std::move(parsed);
            fingerprint = after;
            return true;
        }
    }
    error = "Mod files changed repeatedly while they were being read";
    return false;
}

/** Erases one ID from activation order while preserving all other ordering. */
void EraseEnabled(std::vector<ModId>& order, const ModId& id) {
    order.erase(std::remove(order.begin(), order.end(), id), order.end());
}

/** Collects all exact resource identities changed by one mod definition. */
[[nodiscard]] std::set<core::Tgi> CollectTargets(
    const ModDefinition& definition) {
    std::set<core::Tgi> result;
    for (const ResourcePatchDefinition& patch : definition.patches) {
        result.insert(patch.target);
    }
    return result;
}

}  // namespace

ModManager::ModManager(
    std::filesystem::path modsDirectory,
    std::filesystem::path statePath,
    PatchRegistry& registry,
    runtime::RuntimeResourceCache& cache,
    RuntimeResourceRefreshCallback runtimeRefresh)
    : modsDirectory_(std::move(modsDirectory)),
      statePath_(std::move(statePath)),
      registry_(registry),
      cache_(cache),
      runtimeRefresh_(std::move(runtimeRefresh)) {}

bool ModManager::Initialize(std::string& error) noexcept {
    try {
        const std::scoped_lock lock(mutex_);
        error.clear();
        if (initialized_) {
            error = "Mod manager is already initialized";
            return false;
        }

        std::vector<ModId> requested;
        std::string stateError;
        const bool stateValid = LoadEnabledState(statePath_, requested, stateError);
        enabledOrder_.clear();
        std::string refreshError;
        const bool refreshValid = RefreshLocked(refreshError);
        if (!stateValid) {
            AppendError(error, stateError);
        }
        if (!refreshValid) {
            AppendError(error, refreshError);
        }

        if (stateValid) {
            for (const ModId& id : requested) {
                auto record = records_.find(id);
                if (record == records_.end() || record->second.state != ModState::Inactive) {
                    AppendError(error, "Persisted mod '" + id + "' is unavailable");
                    continue;
                }
                std::string activationError;
                if (!EnableLocked(record->second, false, activationError)) {
                    record->second.state = ModState::Failed;
                    record->second.diagnostic = activationError;
                    AppendError(error, "Mod '" + id + "': " + activationError);
                }
            }
        }
        std::string persistenceError;
        if (!PersistOrder(enabledOrder_, persistenceError)) {
            AppendError(error, persistenceError);
        }
        initialized_ = true;
        return error.empty();
    } catch (...) {
        error = "Mod manager initialization failed due to an allocation or I/O exception";
        return false;
    }
}

bool ModManager::Refresh(std::string& error) noexcept {
    try {
        const std::scoped_lock lock(mutex_);
        if (!initialized_) {
            error = "Mod manager must be initialized before refresh";
            return false;
        }
        return RefreshLocked(error);
    } catch (...) {
        error = "Mod refresh failed due to an allocation or I/O exception";
        return false;
    }
}

bool ModManager::RefreshLocked(std::string& error) {
    error.clear();
    std::error_code directoryError;
    std::filesystem::create_directories(modsDirectory_, directoryError);
    if (directoryError) {
        error = "Could not create or access the mods directory";
        return false;
    }

    failedDiscoveries_.clear();
    std::map<std::filesystem::path, std::string> failedPaths;
    std::vector<std::filesystem::path> directories;
    for (std::filesystem::directory_iterator iterator(modsDirectory_, directoryError), end;
         !directoryError && iterator != end; iterator.increment(directoryError)) {
        std::error_code statusError;
        const std::filesystem::file_status status = iterator->symlink_status(statusError);
        if (statusError) {
            failedDiscoveries_.push_back(ModUiEntry{
                {}, DisplayPath(iterator->path()), {}, {}, ModState::Failed,
                false, false, false, "Could not inspect directory entry"});
            continue;
        }
        const DWORD attributes = GetFileAttributesW(iterator->path().c_str());
        if (std::filesystem::is_symlink(status) ||
            (attributes != INVALID_FILE_ATTRIBUTES &&
             (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0U)) {
            failedDiscoveries_.push_back(ModUiEntry{
                {}, DisplayPath(iterator->path()), {}, {}, ModState::Failed,
                false, false, false, "Symbolic links and reparse targets are not loaded"});
            continue;
        }
        if (std::filesystem::is_directory(status)) {
            directories.push_back(iterator->path());
        }
    }
    if (directoryError) {
        error = "Could not enumerate the mods directory";
        return false;
    }
    std::sort(directories.begin(), directories.end());

    std::map<ModId, std::vector<Candidate>, std::less<>> grouped;
    for (const std::filesystem::path& directory : directories) {
        Candidate candidate;
        std::string candidateError;
        if (!LoadStableCandidate(
                directory, candidate.definition, candidate.fingerprint, candidateError)) {
            failedPaths.emplace(directory, candidateError);
            failedDiscoveries_.push_back(ModUiEntry{
                {}, DisplayPath(directory), {}, {}, ModState::Failed,
                false, false, false, std::move(candidateError)});
            continue;
        }
        grouped[candidate.definition.manifest.id].push_back(std::move(candidate));
    }

    std::map<ModId, Candidate, std::less<>> unique;
    std::set<ModId, std::less<>> duplicateIds;
    for (auto& [id, candidates] : grouped) {
        if (candidates.size() != 1U) {
            duplicateIds.insert(id);
            std::string paths;
            for (const Candidate& candidate : candidates) {
                if (!paths.empty()) {
                    paths += "; ";
                }
                paths += candidate.definition.directory.string();
            }
            for (const Candidate& candidate : candidates) {
                failedDiscoveries_.push_back(ModUiEntry{
                    id, candidate.definition.manifest.name,
                    candidate.definition.manifest.version,
                    candidate.definition.manifest.author, ModState::Failed,
                    false, false, false,
                    "Duplicate mod ID '" + id + "' paths: " + paths});
            }
            continue;
        }
        unique.emplace(id, std::move(candidates.front()));
    }

    bool persistenceChanged = false;
    for (auto iterator = records_.begin(); iterator != records_.end();) {
        auto candidate = unique.find(iterator->first);
        Record& record = iterator->second;
        const bool registered = registry_.IsActive(iterator->first);
        if (candidate == unique.end()) {
            if (registered) {
                const auto failedPath = failedPaths.find(record.definition.directory);
                if (failedPath != failedPaths.end() || duplicateIds.contains(iterator->first)) {
                    record.state = ModState::Changed;
                    record.pendingDefinition.reset();
                    record.pendingFingerprint.reset();
                    record.diagnostic = failedPath != failedPaths.end()
                        ? "Changed files are invalid; active definition retained: " +
                              failedPath->second
                        : "Duplicate mod ID detected; active definition retained until OFF";
                    ++iterator;
                    continue;
                }
                std::set<core::Tgi> affected = CollectTargets(record.definition);
                const std::size_t invalidatedBefore = cache_.Invalidate(affected);
                static_cast<void>(invalidatedBefore);
                std::string deactivateError;
                if (registry_.Deactivate(iterator->first, affected, deactivateError)) {
                    const std::size_t invalidatedAfter = cache_.Invalidate(affected);
                    static_cast<void>(invalidatedAfter);
                    EraseEnabled(enabledOrder_, iterator->first);
                    persistenceChanged = true;
                    record.state = ModState::Missing;
                    record.pendingDefinition.reset();
                    record.pendingFingerprint.reset();
                    record.diagnostic = "Active mod directory is missing or no longer valid";
                    RefreshRuntimeResourcesLocked(
                        affected, record, "removed-mod restoration");
                    ++iterator;
                    continue;
                }
                record.state = ModState::Failed;
                record.diagnostic = "Failed to unload missing mod: " + deactivateError;
                AppendError(error, record.diagnostic);
                ++iterator;
                continue;
            }
            iterator = records_.erase(iterator);
            continue;
        }

        if (registered) {
            if (record.fingerprint != candidate->second.fingerprint) {
                record.pendingDefinition = std::move(candidate->second.definition);
                record.pendingFingerprint = candidate->second.fingerprint;
                record.state = ModState::Changed;
                record.diagnostic =
                    "Files changed while active; switch OFF and ON to load the new version";
            } else {
                record.pendingDefinition.reset();
                record.pendingFingerprint.reset();
                record.state = ModState::Active;
                record.diagnostic.clear();
            }
        } else {
            record.definition = std::move(candidate->second.definition);
            record.fingerprint = candidate->second.fingerprint;
            record.pendingDefinition.reset();
            record.pendingFingerprint.reset();
            record.state = ModState::Inactive;
            record.diagnostic.clear();
        }
        unique.erase(candidate);
        ++iterator;
    }

    for (auto& [id, candidate] : unique) {
        Record record;
        record.definition = std::move(candidate.definition);
        record.fingerprint = candidate.fingerprint;
        record.state = ModState::Inactive;
        records_.emplace(id, std::move(record));
    }
    if (persistenceChanged) {
        std::string persistenceError;
        if (!PersistOrder(enabledOrder_, persistenceError)) {
            AppendError(error, persistenceError);
        }
    }
    return error.empty();
}

bool ModManager::EnableLocked(
    Record& record,
    bool persist,
    std::string& error) {
    error.clear();
    if (record.state != ModState::Inactive) {
        error = "Only an inactive mod can be enabled";
        return false;
    }
    const ModId& id = record.definition.manifest.id;
    const std::vector<ModId> previousOrder = enabledOrder_;
    std::vector<ModId> candidateOrder = previousOrder;
    candidateOrder.push_back(id);
    record.state = ModState::Loading;
    if (persist && !PersistOrder(candidateOrder, error)) {
        record.state = ModState::Inactive;
        record.diagnostic = error;
        return false;
    }
    std::set<core::Tgi> affected = CollectTargets(record.definition);
    const std::size_t invalidatedBefore = cache_.Invalidate(affected);
    static_cast<void>(invalidatedBefore);
    if (!registry_.Activate(id, record.definition.patches, affected, error)) {
        if (persist) {
            std::string rollbackError;
            if (!PersistOrder(previousOrder, rollbackError)) {
                error += "; persistence rollback failed: " + rollbackError;
            }
        }
        record.state = ModState::Failed;
        record.diagnostic = error;
        return false;
    }
    const std::size_t invalidatedAfter = cache_.Invalidate(affected);
    static_cast<void>(invalidatedAfter);
    enabledOrder_ = std::move(candidateOrder);
    record.state = ModState::Active;
    record.diagnostic.clear();
    RefreshRuntimeResourcesLocked(affected, record, "enable");
    return true;
}

bool ModManager::DisableLocked(
    Record& record,
    bool persist,
    std::string& error) {
    error.clear();
    if (!registry_.IsActive(record.definition.manifest.id)) {
        error = "Only an active or changed mod can be disabled";
        return false;
    }
    const ModId id = record.definition.manifest.id;
    const std::vector<ModId> previousOrder = enabledOrder_;
    std::vector<ModId> candidateOrder = previousOrder;
    EraseEnabled(candidateOrder, id);
    const ModState previousState = record.state;
    record.state = ModState::Unloading;
    if (persist && !PersistOrder(candidateOrder, error)) {
        record.state = previousState;
        record.diagnostic = error;
        return false;
    }
    std::set<core::Tgi> affected = CollectTargets(record.definition);
    const std::size_t invalidatedBefore = cache_.Invalidate(affected);
    static_cast<void>(invalidatedBefore);
    if (!registry_.Deactivate(id, affected, error)) {
        if (persist) {
            std::string rollbackError;
            if (!PersistOrder(previousOrder, rollbackError)) {
                error += "; persistence rollback failed: " + rollbackError;
            }
        }
        record.state = previousState;
        record.diagnostic = error;
        return false;
    }
    const std::size_t invalidatedAfter = cache_.Invalidate(affected);
    static_cast<void>(invalidatedAfter);
    enabledOrder_ = std::move(candidateOrder);
    const bool invalidChangedDefinition =
        previousState == ModState::Changed &&
        (!record.pendingDefinition.has_value() ||
         !record.pendingFingerprint.has_value());
    if (record.pendingDefinition.has_value() && record.pendingFingerprint.has_value()) {
        record.definition = std::move(*record.pendingDefinition);
        record.fingerprint = *record.pendingFingerprint;
        record.pendingDefinition.reset();
        record.pendingFingerprint.reset();
    }
    if (invalidChangedDefinition) {
        record.state = ModState::Failed;
        if (record.diagnostic.empty()) {
            record.diagnostic =
                "Changed files must validate before this mod can be enabled again";
        }
    } else {
        record.state = ModState::Inactive;
        record.diagnostic.clear();
    }
    RefreshRuntimeResourcesLocked(affected, record, "disable");
    return true;
}

void ModManager::RefreshRuntimeResourcesLocked(
    const std::set<core::Tgi>& affected,
    Record& record,
    std::string_view action) noexcept {
    if (!runtimeRefresh_ || affected.empty()) {
        return;
    }
    try {
        std::string refreshError;
        if (!runtimeRefresh_(affected, refreshError)) {
            const std::string liveDiagnostic = "Registry state committed, but live " +
                std::string(action) + " failed: " +
                (refreshError.empty() ? "unspecified runtime refresh error" : refreshError);
            AppendError(record.diagnostic, liveDiagnostic);
        }
    } catch (...) {
        AppendError(
            record.diagnostic,
            "Registry state committed, but live " + std::string(action) +
                " failed with an exception");
    }
}

bool ModManager::SetEnabled(
    const ModId& id,
    bool enabled,
    std::string& error) noexcept {
    try {
        const std::scoped_lock lock(mutex_);
        if (!initialized_) {
            error = "Mod manager is not initialized";
            return false;
        }
        if (enabled) {
            std::string refreshError;
            if (!RefreshLocked(refreshError)) {
                error = "Could not refresh mod files before enable: " + refreshError;
                return false;
            }
        }
        const auto record = records_.find(id);
        if (record == records_.end()) {
            error = "Unknown mod ID '" + id + "'";
            return false;
        }
        return enabled ? EnableLocked(record->second, true, error)
                       : DisableLocked(record->second, true, error);
    } catch (...) {
        error = "Mod state change failed due to an allocation or I/O exception";
        return false;
    }
}

std::vector<ModUiEntry> ModManager::Snapshot() const {
    const std::scoped_lock lock(mutex_);
    std::vector<ModUiEntry> result;
    result.reserve(records_.size() + failedDiscoveries_.size());
    for (const auto& [id, record] : records_) {
        const bool registered = registry_.IsActive(id);
        result.push_back(ModUiEntry{
            id,
            record.definition.manifest.name,
            record.definition.manifest.version,
            record.definition.manifest.author,
            record.state,
            record.state == ModState::Inactive && !registered,
            registered,
            record.state == ModState::Changed,
            record.diagnostic});
    }
    result.insert(result.end(), failedDiscoveries_.begin(), failedDiscoveries_.end());
    return result;
}

std::vector<ModId> ModManager::EnabledOrder() const {
    const std::scoped_lock lock(mutex_);
    return enabledOrder_;
}

std::vector<PatchConflict> ModManager::Conflicts() const {
    return registry_.Current()->conflicts();
}

bool ModManager::PersistOrder(
    const std::vector<ModId>& order,
    std::string& error) const {
    return SaveEnabledState(statePath_, order, error);
}

}  // namespace sc13::mods
