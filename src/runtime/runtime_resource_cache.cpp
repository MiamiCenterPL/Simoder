#include "runtime/runtime_resource_cache.hpp"

#include "mods/patch_registry.hpp"

#include <algorithm>
#include <chrono>

namespace sc13::runtime {
namespace {

/** Compares complete runtime records without relying on structure padding. */
[[nodiscard]] bool EqualRecords(
    std::span<const core::RuntimePropertyRecord> left,
    std::span<const core::RuntimePropertyRecord> right) noexcept {
    return left.size() == right.size() &&
           std::equal(
               left.begin(), left.end(), right.begin(),
               [](const core::RuntimePropertyRecord& first,
                  const core::RuntimePropertyRecord& second) {
                   return first.id == second.id &&
                          first.valueBits == second.valueBits &&
                          first.auxiliary == second.auxiliary &&
                          first.metadata == second.metadata;
               });
}

}  // namespace

bool RuntimeResourceCache::Prepare(
    const RuntimeResourceKey& key,
    std::span<const core::RuntimePropertyRecord> current,
    mods::PatchGeneration generation,
    std::span<const mods::RegisteredPatch> patches,
    RuntimeResourceBuildPlan& plan,
    std::string& error) noexcept {
    try {
        plan = {};
        error.clear();
        if (key.resourceAddress == 0U ||
            core::ValidateRuntimePropertyTable(current, current.size()) !=
                core::RuntimeTableStatus::Valid) {
            error = "Runtime resource cache rejected its identity or property table";
            return false;
        }

        std::vector<core::RuntimePropertyRecord> vanilla;
        bool hadExisting = false;
        {
            const std::scoped_lock lock(mutex_);
            const auto epoch = invalidationEpochs_.find(key.target);
            plan.invalidationEpoch =
                epoch == invalidationEpochs_.end() ? 0U : epoch->second;
            const auto existing = entries_.find(key);
            if (existing != entries_.end()) {
                hadExisting = true;
                const bool matchesVanilla = EqualRecords(current, existing->second.vanilla);
                const bool matchesApplied = EqualRecords(current, existing->second.lastApplied);
                if (!matchesVanilla && !matchesApplied) {
                    error =
                        "Current resource differs from both retained vanilla and last result";
                    return false;
                }
                vanilla = existing->second.vanilla;
                if (existing->second.valid) {
                    plan.key = key;
                    plan.generation = generation;
                    plan.status = RuntimeResourceBuildStatus::Reused;
                    plan.vanilla = existing->second.vanilla;
                    plan.previous.assign(current.begin(), current.end());
                    plan.desired = existing->second.lastApplied;
                    return true;
                }
            } else {
                vanilla.assign(current.begin(), current.end());
            }
        }

        std::vector<core::RuntimePropertyRecord> desired;
        if (!mods::ApplyPatchSequence(key.target, vanilla, patches, desired, error)) {
            return false;
        }
        plan.key = key;
        plan.generation = generation;
        plan.vanilla = std::move(vanilla);
        plan.previous.assign(current.begin(), current.end());
        plan.desired = std::move(desired);
        if (!hadExisting) {
            plan.status = RuntimeResourceBuildStatus::Created;
        } else if (EqualRecords(plan.desired, plan.vanilla) &&
                   !EqualRecords(plan.previous, plan.vanilla)) {
            plan.status = RuntimeResourceBuildStatus::Restored;
        } else {
            plan.status = RuntimeResourceBuildStatus::Rebuilt;
        }
        return true;
    } catch (...) {
        plan = {};
        error = "Runtime resource cache prepare failed due to an allocation exception";
        return false;
    }
}

bool RuntimeResourceCache::Commit(
    const RuntimeResourceBuildPlan& plan,
    std::string& error) noexcept {
    try {
        error.clear();
        if (plan.key.resourceAddress == 0U || plan.vanilla.empty() ||
            plan.vanilla.size() != plan.desired.size()) {
            error = "Runtime resource cache commit received an invalid plan";
            return false;
        }
        const std::scoped_lock lock(mutex_);
        const auto epoch = invalidationEpochs_.find(plan.key.target);
        const std::uint64_t currentEpoch =
            epoch == invalidationEpochs_.end() ? 0U : epoch->second;
        if (currentEpoch != plan.invalidationEpoch) {
            error = "Runtime resource cache plan was invalidated before commit";
            return false;
        }
        const auto existing = entries_.find(plan.key);
        if (existing != entries_.end() &&
            !EqualRecords(existing->second.vanilla, plan.vanilla)) {
            error = "Runtime resource cache vanilla changed before commit";
            return false;
        }
        Entry committed;
        committed.vanilla = plan.vanilla;
        committed.lastApplied = plan.desired;
        committed.generation = plan.generation;
        committed.valid = true;
        entries_.insert_or_assign(plan.key, std::move(committed));
        try {
            if (observations_.size() >= 128U && !observations_.contains(plan.key)) observations_.erase(observations_.begin());
            observations_.insert_or_assign(plan.key, RuntimeResourceObservation{plan.key, plan.generation,
                EqualRecords(plan.vanilla, plan.desired) ? "observed_unchanged" : "applied", "", plan.vanilla, plan.desired,
                static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::system_clock::now().time_since_epoch()).count()), plan.generation});
        } catch (...) { /* @summary Diagnostic allocation failure must not invalidate a committed memory transaction. */ }
        return true;
    } catch (...) {
        error = "Runtime resource cache commit failed due to an allocation exception";
        return false;
    }
}

std::size_t RuntimeResourceCache::Invalidate(
    const std::set<core::Tgi>& affected) noexcept {
    try {
        const std::scoped_lock lock(mutex_);
        std::size_t invalidated = 0U;
        for (const core::Tgi& target : affected) {
            ++invalidationEpochs_[target];
            for (auto& [key, observation] : observations_)
                if (key.target == target) observation.status = "pending";
        }
        for (auto& [key, entry] : entries_) {
            if (entry.valid && affected.contains(key.target)) {
                entry.valid = false;
                ++invalidated;
            }
        }
        return invalidated;
    } catch (...) {
        return 0U;
    }
}

bool RuntimeResourceCache::Contains(const RuntimeResourceKey& key) const {
    const std::scoped_lock lock(mutex_);
    return entries_.contains(key);
}

std::size_t RuntimeResourceCache::size() const {
    const std::scoped_lock lock(mutex_);
    return entries_.size();
}

std::vector<RuntimeResourceRestoreEntry>
RuntimeResourceCache::SnapshotForRestore() const {
    const std::scoped_lock lock(mutex_);
    std::vector<RuntimeResourceRestoreEntry> result;
    result.reserve(entries_.size());
    for (const auto& [key, entry] : entries_) {
        result.push_back(RuntimeResourceRestoreEntry{
            key, entry.vanilla, entry.lastApplied});
    }
    return result;
}

void RuntimeResourceCache::Clear() noexcept {
    try {
        const std::scoped_lock lock(mutex_);
        entries_.clear();
        invalidationEpochs_.clear();
        observations_.clear();
    } catch (...) {
    }
}

void RuntimeResourceCache::RecordFailure(const RuntimeResourceKey& key, mods::PatchGeneration generation,
    std::string_view reason) noexcept {
    try {
        const std::scoped_lock lock(mutex_);
        if (observations_.size() >= 128U && !observations_.contains(key)) observations_.erase(observations_.begin());
        auto& observation = observations_[key];
        observation.key = key; observation.generation = generation;
        observation.status = "rejected"; observation.diagnostic = reason;
        observation.observedAtUnixMs = static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count());
    } catch (...) { /* @summary Observation failure leaves patch safety and gameplay unchanged. */ }
}

std::vector<RuntimeResourceObservation> RuntimeResourceCache::Observations() const {
    const std::scoped_lock lock(mutex_);
    std::vector<RuntimeResourceObservation> result;
    for (const auto& [key, observation] : observations_) { static_cast<void>(key); result.push_back(observation); }
    return result;
}

const char* RuntimeResourceBuildStatusName(
    RuntimeResourceBuildStatus status) noexcept {
    switch (status) {
        case RuntimeResourceBuildStatus::Created: return "created";
        case RuntimeResourceBuildStatus::Rebuilt: return "rebuilt";
        case RuntimeResourceBuildStatus::Reused: return "reused";
        case RuntimeResourceBuildStatus::Restored: return "restored";
    }
    return "unknown";
}

}  // namespace sc13::runtime
