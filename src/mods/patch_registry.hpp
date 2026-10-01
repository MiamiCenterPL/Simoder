#pragma once

#include "core/runtime_property_table.hpp"
#include "mods/mod_types.hpp"

#include <atomic>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <span>
#include <string>
#include <vector>

namespace sc13::mods {

/** Exposes one immutable generation of active TGI-indexed patch definitions. */
class PatchRegistrySnapshot final {
public:
    /** Returns the snapshot generation. */
    [[nodiscard]] PatchGeneration generation() const noexcept { return generation_; }

    /** Returns patches for one TGI without scanning unrelated mods. */
    [[nodiscard]] const std::vector<RegisteredPatch>* Find(
        const core::Tgi& target) const noexcept;

    /** Returns conflicts detected while this snapshot was built. */
    [[nodiscard]] const std::vector<PatchConflict>& conflicts() const noexcept {
        return conflicts_;
    }

private:
    friend class PatchRegistry;
    PatchGeneration generation_{};
    std::map<core::Tgi, std::vector<RegisteredPatch>> patches_;
    std::vector<PatchConflict> conflicts_;
};

/** Owns active mod patches and atomically publishes immutable hot-path snapshots. */
class PatchRegistry final {
public:
    /** Creates an empty generation-zero registry. */
    PatchRegistry();

    /** Transactionally registers all patches owned by one inactive mod. */
    [[nodiscard]] bool Activate(
        const ModId& owner,
        std::span<const ResourcePatchDefinition> patches,
        std::set<core::Tgi>& affected,
        std::string& error) noexcept;

    /** Transactionally removes all patches owned by one active mod. */
    [[nodiscard]] bool Deactivate(
        const ModId& owner,
        std::set<core::Tgi>& affected,
        std::string& error) noexcept;
    /** @summary Replaces an active owner's patches in one publication while preserving activation order. */
    [[nodiscard]] bool Replace(const ModId& owner,
        std::span<const ResourcePatchDefinition> patches,
        std::set<core::Tgi>& affected, std::string& error) noexcept;

    /** Atomically loads the current immutable snapshot. */
    [[nodiscard]] std::shared_ptr<const PatchRegistrySnapshot> Current() const noexcept;

    /** Reports whether an owner currently has a committed patch set. */
    [[nodiscard]] bool IsActive(const ModId& owner) const;

private:
    /** Stores one complete active mod patch set before snapshot expansion. */
    struct OwnedPatchSet final {
        std::uint64_t sequence{};
        std::vector<ResourcePatchDefinition> patches;
    };

    using ActivePatchSets = std::map<ModId, OwnedPatchSet, std::less<>>;

    /** Builds one deterministic immutable snapshot from candidate active sets. */
    [[nodiscard]] static std::shared_ptr<PatchRegistrySnapshot> BuildSnapshot(
        const ActivePatchSets& active,
        PatchGeneration generation);

    mutable std::mutex mutationMutex_;
    ActivePatchSets active_;
    std::uint64_t nextSequence_{1U};
    PatchGeneration generation_{};
    std::atomic<std::shared_ptr<const PatchRegistrySnapshot>> current_;
};

/** Rebuilds one runtime PROP table from vanilla plus an ordered patch list. */
[[nodiscard]] bool ApplyPatchSequence(
    const core::Tgi& target,
    std::span<const core::RuntimePropertyRecord> vanilla,
    std::span<const RegisteredPatch> patches,
    std::vector<core::RuntimePropertyRecord>& result,
    std::string& error) noexcept;

}  // namespace sc13::mods
