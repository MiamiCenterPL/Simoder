#include "mods/patch_registry.hpp"

#include <algorithm>
#include <bit>
#include <cmath>
#include <limits>
#include <utility>

namespace sc13::mods {
namespace {

constexpr std::uint32_t kScalarFloatMetadata = 0x000D0000U;

/** Applies one finite float operation without changing the source on failure. */
[[nodiscard]] bool ApplyFloatOperation(
    float current,
    const PropertyPatch& patch,
    float& result) noexcept {
    switch (patch.operation) {
        case PatchOperation::Set: result = patch.value; break;
        case PatchOperation::Add: result = current + patch.value; break;
        case PatchOperation::Subtract: result = current - patch.value; break;
        case PatchOperation::Multiply: result = current * patch.value; break;
        case PatchOperation::Divide:
            if (patch.value == 0.0F) {
                return false;
            }
            result = current / patch.value;
            break;
    }
    return std::isfinite(result);
}

/** Sorts resource blocks by activation sequence with owner as a stable tie-breaker. */
[[nodiscard]] bool RegisteredPatchLess(
    const RegisteredPatch& left,
    const RegisteredPatch& right) noexcept {
    if (left.sequence != right.sequence) {
        return left.sequence < right.sequence;
    }
    return left.owner < right.owner;
}

}  // namespace

const std::vector<RegisteredPatch>* PatchRegistrySnapshot::Find(
    const core::Tgi& target) const noexcept {
    const auto iterator = patches_.find(target);
    return iterator == patches_.end() ? nullptr : &iterator->second;
}

PatchRegistry::PatchRegistry() {
    auto initial = std::make_shared<PatchRegistrySnapshot>();
    current_.store(std::move(initial), std::memory_order_release);
}

std::shared_ptr<PatchRegistrySnapshot> PatchRegistry::BuildSnapshot(
    const ActivePatchSets& active,
    PatchGeneration generation) {
    auto snapshot = std::make_shared<PatchRegistrySnapshot>();
    snapshot->generation_ = generation;
    for (const auto& [owner, owned] : active) {
        for (const ResourcePatchDefinition& patch : owned.patches) {
            snapshot->patches_[patch.target].push_back(
                RegisteredPatch{owner, patch, owned.sequence});
        }
    }
    for (auto& [target, patches] : snapshot->patches_) {
        std::sort(patches.begin(), patches.end(), RegisteredPatchLess);
        std::map<std::uint32_t, ModId> previousOwners;
        for (const RegisteredPatch& registered : patches) {
            for (const PropertyPatch& property : registered.patch.properties) {
                const auto previous = previousOwners.find(property.propertyId);
                if (previous != previousOwners.end() &&
                    previous->second != registered.owner) {
                    snapshot->conflicts_.push_back(PatchConflict{
                        target, property.propertyId, previous->second, registered.owner});
                }
                previousOwners[property.propertyId] = registered.owner;
            }
        }
    }
    return snapshot;
}

bool PatchRegistry::Activate(
    const ModId& owner,
    std::span<const ResourcePatchDefinition> patches,
    std::set<core::Tgi>& affected,
    std::string& error) noexcept {
    try {
        affected.clear();
        error.clear();
        if (owner.empty() || patches.empty()) {
            error = "Patch activation requires an owner and at least one patch";
            return false;
        }
        const std::scoped_lock lock(mutationMutex_);
        if (active_.contains(owner)) {
            error = "Patch owner is already active";
            return false;
        }
        ActivePatchSets candidate = active_;
        OwnedPatchSet owned;
        owned.sequence = nextSequence_;
        owned.patches.assign(patches.begin(), patches.end());
        candidate.emplace(owner, std::move(owned));
        const PatchGeneration nextGeneration = generation_ + 1U;
        std::shared_ptr<PatchRegistrySnapshot> snapshot =
            BuildSnapshot(candidate, nextGeneration);
        for (const ResourcePatchDefinition& patch : patches) {
            affected.insert(patch.target);
        }
        active_ = std::move(candidate);
        ++nextSequence_;
        generation_ = nextGeneration;
        current_.store(std::move(snapshot), std::memory_order_release);
        return true;
    } catch (...) {
        affected.clear();
        error = "Patch activation failed transactionally due to an allocation exception";
        return false;
    }
}

bool PatchRegistry::Deactivate(
    const ModId& owner,
    std::set<core::Tgi>& affected,
    std::string& error) noexcept {
    try {
        affected.clear();
        error.clear();
        const std::scoped_lock lock(mutationMutex_);
        const auto existing = active_.find(owner);
        if (existing == active_.end()) {
            error = "Patch owner is not active";
            return false;
        }
        for (const ResourcePatchDefinition& patch : existing->second.patches) {
            affected.insert(patch.target);
        }
        ActivePatchSets candidate = active_;
        candidate.erase(owner);
        const PatchGeneration nextGeneration = generation_ + 1U;
        std::shared_ptr<PatchRegistrySnapshot> snapshot =
            BuildSnapshot(candidate, nextGeneration);
        active_ = std::move(candidate);
        generation_ = nextGeneration;
        current_.store(std::move(snapshot), std::memory_order_release);
        return true;
    } catch (...) {
        affected.clear();
        error = "Patch deactivation failed transactionally due to an allocation exception";
        return false;
    }
}

std::shared_ptr<const PatchRegistrySnapshot> PatchRegistry::Current() const noexcept {
    return current_.load(std::memory_order_acquire);
}

bool PatchRegistry::Replace(const ModId& owner, std::span<const ResourcePatchDefinition> patches,
    std::set<core::Tgi>& affected, std::string& error) noexcept {
    try {
        error.clear(); affected.clear();
        const std::scoped_lock lock(mutationMutex_);
        const auto existing = active_.find(owner);
        if (existing == active_.end() || patches.empty()) { error = "Replace requires an active owner and patches"; return false; }
        auto candidate = active_;
        for (const auto& patch : existing->second.patches) affected.insert(patch.target);
        for (const auto& patch : patches) affected.insert(patch.target);
        candidate.at(owner).patches.assign(patches.begin(), patches.end());
        const auto nextGeneration = generation_ + 1U;
        auto snapshot = BuildSnapshot(candidate, nextGeneration);
        active_ = std::move(candidate);
        generation_ = nextGeneration;
        current_.store(std::move(snapshot), std::memory_order_release);
        return true;
    } catch (...) { affected.clear(); error = "Patch replacement failed before publication"; return false; }
}

bool PatchRegistry::IsActive(const ModId& owner) const {
    const std::scoped_lock lock(mutationMutex_);
    return active_.contains(owner);
}

bool ApplyPatchSequence(
    const core::Tgi& target,
    std::span<const core::RuntimePropertyRecord> vanilla,
    std::span<const RegisteredPatch> patches,
    std::vector<core::RuntimePropertyRecord>& result,
    std::string& error) noexcept {
    try {
        error.clear();
        if (core::ValidateRuntimePropertyTable(vanilla, vanilla.size()) !=
            core::RuntimeTableStatus::Valid) {
            error = "Vanilla runtime property table is malformed";
            return false;
        }
        std::vector<core::RuntimePropertyRecord> candidate(vanilla.begin(), vanilla.end());
        for (const RegisteredPatch& registered : patches) {
            if (registered.owner.empty() || registered.patch.target != target) {
                error = "Registered patch ownership or target is invalid";
                return false;
            }
            for (const PropertyPatch& patch : registered.patch.properties) {
                const auto property = std::lower_bound(
                    candidate.begin(), candidate.end(), patch.propertyId,
                    [](const core::RuntimePropertyRecord& entry, std::uint32_t identifier) {
                        return entry.id < identifier;
                    });
                if (property == candidate.end() || property->id != patch.propertyId) {
                    error = "Mod '" + registered.owner +
                            "' targets a property missing from the vanilla resource";
                    return false;
                }
                if (patch.type != PropertyType::Float ||
                    property->metadata != kScalarFloatMetadata) {
                    error = "Mod '" + registered.owner +
                            "' targets a property with an incompatible runtime type";
                    return false;
                }
                const float current = std::bit_cast<float>(property->valueBits);
                float replacement = 0.0F;
                if (!std::isfinite(current) ||
                    !ApplyFloatOperation(current, patch, replacement)) {
                    error = "Mod '" + registered.owner +
                            "' produced a non-finite or invalid float result";
                    return false;
                }
                property->valueBits = std::bit_cast<std::uint32_t>(replacement);
            }
        }
        result = std::move(candidate);
        return true;
    } catch (...) {
        result.clear();
        error = "Patch sequence failed due to an allocation exception";
        return false;
    }
}

}  // namespace sc13::mods
