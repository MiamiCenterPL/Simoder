#pragma once

#include "core/runtime_property_table.hpp"
#include "core/tgi.hpp"
#include "mods/mod_types.hpp"

#include <compare>
#include <cstdint>
#include <map>
#include <mutex>
#include <set>
#include <span>
#include <string>
#include <vector>

namespace sc13::runtime {

/** Uniquely identifies one game-owned runtime resource instance. */
struct RuntimeResourceKey final {
    core::Tgi target{};
    std::uintptr_t resourceAddress{};

    auto operator<=>(const RuntimeResourceKey&) const = default;
};

/** Classifies how a prepared generation relates to the retained resource state. */
enum class RuntimeResourceBuildStatus {
    Created,
    Rebuilt,
    Reused,
    Restored
};

/** Owns all data needed for a transactional game-memory update. */
struct RuntimeResourceBuildPlan final {
    RuntimeResourceKey key{};
    mods::PatchGeneration generation{};
    std::uint64_t invalidationEpoch{};
    RuntimeResourceBuildStatus status{RuntimeResourceBuildStatus::Created};
    std::vector<core::RuntimePropertyRecord> vanilla;
    std::vector<core::RuntimePropertyRecord> previous;
    std::vector<core::RuntimePropertyRecord> desired;
};

/** Copies retained values needed to restore one live resource during controlled detach. */
struct RuntimeResourceRestoreEntry final {
    RuntimeResourceKey key{};
    std::vector<core::RuntimePropertyRecord> vanilla;
    std::vector<core::RuntimePropertyRecord> lastApplied;
};

/** @summary Captures observed resource outcomes; successful cache data is historical, not a live pointer read. */
struct RuntimeResourceObservation final {
    RuntimeResourceKey key{};
    mods::PatchGeneration generation{};
    std::string status;
    std::string diagnostic;
    std::vector<core::RuntimePropertyRecord> vanilla;
    std::vector<core::RuntimePropertyRecord> applied;
    std::uint64_t observedAtUnixMs{};
    /** @summary Distinguishes the last successful value snapshot from a later rejected attempt. */
    mods::PatchGeneration committedGeneration{};
};

/** Retains reproducible vanilla-derived resource generations without lending pointers to the game. */
class RuntimeResourceCache final {
public:
    /** Builds a candidate result from retained vanilla plus the supplied ordered patches. */
    [[nodiscard]] bool Prepare(
        const RuntimeResourceKey& key,
        std::span<const core::RuntimePropertyRecord> current,
        mods::PatchGeneration generation,
        std::span<const mods::RegisteredPatch> patches,
        RuntimeResourceBuildPlan& plan,
        std::string& error) noexcept;

    /** Commits one plan only after the caller has atomically written every desired value. */
    [[nodiscard]] bool Commit(
        const RuntimeResourceBuildPlan& plan,
        std::string& error) noexcept;

    /** Marks only entries for affected TGIs stale and returns their count. */
    [[nodiscard]] std::size_t Invalidate(
        const std::set<core::Tgi>& affected) noexcept;

    /** Reports whether one game-owned instance has retained vanilla state. */
    [[nodiscard]] bool Contains(const RuntimeResourceKey& key) const;

    /** Returns the number of retained game-owned resource identities. */
    [[nodiscard]] std::size_t size() const;

    /** Returns owned restoration snapshots without exposing internal cache storage. */
    [[nodiscard]] std::vector<RuntimeResourceRestoreEntry> SnapshotForRestore() const;
    /** @summary Records an observed rejection without retaining a dereferenceable game pointer. */
    void RecordFailure(const RuntimeResourceKey& key, mods::PatchGeneration generation,
        std::string_view reason) noexcept;
    /** @summary Returns bounded historical evidence for UI and external tools. */
    [[nodiscard]] std::vector<RuntimeResourceObservation> Observations() const;

    /** Releases loader-owned snapshots during controlled shutdown. */
    void Clear() noexcept;

private:
    /** Stores one process-lifetime vanilla snapshot and last committed result. */
    struct Entry final {
        std::vector<core::RuntimePropertyRecord> vanilla;
        std::vector<core::RuntimePropertyRecord> lastApplied;
        mods::PatchGeneration generation{};
        bool valid{};
    };

    mutable std::mutex mutex_;
    std::map<RuntimeResourceKey, Entry> entries_;
    std::map<core::Tgi, std::uint64_t> invalidationEpochs_;
    std::map<RuntimeResourceKey, RuntimeResourceObservation> observations_;
};

/** Returns a stable diagnostic label for a cache build status. */
[[nodiscard]] const char* RuntimeResourceBuildStatusName(
    RuntimeResourceBuildStatus status) noexcept;

}  // namespace sc13::runtime
