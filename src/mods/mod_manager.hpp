#pragma once

#include "core/sha256.hpp"
#include "mods/mod_types.hpp"

#include <filesystem>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace sc13::runtime {
class RuntimeResourceCache;
}

namespace sc13::mods {

class PatchRegistry;

/** Rebuilds retained live resources after a committed registry generation change. */
using RuntimeResourceRefreshCallback = std::function<bool(
    const std::set<core::Tgi>& affected,
    std::string& error)>;

/** Identifies the exact manifest and override bytes used for one definition. */
struct ModDefinitionFingerprint final {
    core::Sha256Digest manifest{};
    core::Sha256Digest overrides{};

    auto operator<=>(const ModDefinitionFingerprint&) const = default;
};

/** Owns discovery, lifecycle, persistence, and UI projections for installed mods. */
class ModManager final {
public:
    /** Binds one game-root mods directory and persistent state file to runtime services. */
    ModManager(
        std::filesystem::path modsDirectory,
        std::filesystem::path statePath,
        PatchRegistry& registry,
        runtime::RuntimeResourceCache& cache,
        RuntimeResourceRefreshCallback runtimeRefresh = {});

    /** Discovers mods and activates each valid persisted entry with failure isolation. */
    [[nodiscard]] bool Initialize(std::string& error) noexcept;

    /** Rescans immediate mod directories without automatically enabling new mods. */
    [[nodiscard]] bool Refresh(std::string& error) noexcept;

    /** Transactionally changes one mod's enabled state and persists activation order. */
    [[nodiscard]] bool SetEnabled(
        const ModId& id,
        bool enabled,
        std::string& error) noexcept;

    /** Returns an immutable copy suitable for rendering outside the manager lock. */
    [[nodiscard]] std::vector<ModUiEntry> Snapshot() const;

    /** Returns the currently persisted deterministic activation order. */
    [[nodiscard]] std::vector<ModId> EnabledOrder() const;

    /** Returns same-property conflicts from the current immutable registry snapshot. */
    [[nodiscard]] std::vector<PatchConflict> Conflicts() const;

private:
    /** Retains the committed definition and an optional changed-on-disk successor. */
    struct Record final {
        ModDefinition definition;
        ModDefinitionFingerprint fingerprint{};
        std::optional<ModDefinition> pendingDefinition;
        std::optional<ModDefinitionFingerprint> pendingFingerprint;
        ModState state{ModState::Discovered};
        std::string diagnostic;
    };

    /** Stores one complete, stable discovery result before manager mutation. */
    struct Candidate final {
        ModDefinition definition;
        ModDefinitionFingerprint fingerprint{};
    };

    /** Performs refresh while the manager lock is already held. */
    [[nodiscard]] bool RefreshLocked(std::string& error);

    /** Activates one inactive record with optional immediate persistence. */
    [[nodiscard]] bool EnableLocked(
        Record& record,
        bool persist,
        std::string& error);

    /** Deactivates one active or changed record with optional immediate persistence. */
    [[nodiscard]] bool DisableLocked(
        Record& record,
        bool persist,
        std::string& error);

    /** Applies the current registry generation to retained live resources. */
    void RefreshRuntimeResourcesLocked(
        const std::set<core::Tgi>& affected,
        Record& record,
        std::string_view action) noexcept;

    /** Persists the supplied activation order to the configured state path. */
    [[nodiscard]] bool PersistOrder(
        const std::vector<ModId>& order,
        std::string& error) const;

    std::filesystem::path modsDirectory_;
    std::filesystem::path statePath_;
    PatchRegistry& registry_;
    runtime::RuntimeResourceCache& cache_;
    RuntimeResourceRefreshCallback runtimeRefresh_;
    mutable std::mutex mutex_;
    std::map<ModId, Record, std::less<>> records_;
    std::vector<ModUiEntry> failedDiscoveries_;
    std::vector<ModId> enabledOrder_;
    bool initialized_{};
};

}  // namespace sc13::mods
