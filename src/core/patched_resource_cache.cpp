#include "core/patched_resource_cache.hpp"

#include <bit>

namespace sc13::core {
namespace {

/** Verifies that a repeated source differs from its patched copy only at the declared value. */
[[nodiscard]] bool MatchesCachedSource(
    std::span<const RuntimePropertyRecord> source,
    std::span<const RuntimePropertyRecord> cached,
    const RuntimeFloatPatch& patch) noexcept {
    if (source.size() != cached.size()) {
        return false;
    }
    const std::uint32_t expectedBits = std::bit_cast<std::uint32_t>(patch.expectedValue);
    const std::uint32_t replacementBits = std::bit_cast<std::uint32_t>(patch.replacementValue);
    for (std::size_t index = 0; index < source.size(); ++index) {
        const RuntimePropertyRecord& incoming = source[index];
        const RuntimePropertyRecord& retained = cached[index];
        if (incoming.id != retained.id || incoming.auxiliary != retained.auxiliary ||
            incoming.metadata != retained.metadata) {
            return false;
        }
        if (incoming.id == patch.identifier) {
            if ((incoming.valueBits != expectedBits && incoming.valueBits != replacementBits) ||
                retained.valueBits != replacementBits) {
                return false;
            }
        } else if (incoming.valueBits != retained.valueBits) {
            return false;
        }
    }
    return true;
}

}  // namespace

bool RuntimeResourceCopy::Create(
    const Tgi& tgi,
    std::span<const RuntimePropertyRecord> source,
    std::size_t expectedCount,
    RuntimeResourceCopy& copy,
    std::string& error) noexcept {
    try {
        if (ValidateRuntimePropertyTable(source, expectedCount) != RuntimeTableStatus::Valid) {
            error = "Runtime resource copy rejected a malformed property table";
            return false;
        }
        RuntimeResourceCopy candidate;
        candidate.tgi_ = tgi;
        candidate.records_.assign(source.begin(), source.end());
        copy = std::move(candidate);
        return true;
    } catch (...) {
        error = "Runtime resource copy allocation failed";
        return false;
    }
}

RuntimeFloatPatchStatus RuntimeResourceCopy::PatchFloat(
    const RuntimeFloatPatch& patch) noexcept {
    return PatchRuntimeFloat(
        records_, records_.size(), patch.identifier, patch.expectedValue,
        patch.replacementValue);
}

const RuntimeResourceCopy* PatchedResourceCache::GetOrCreate(
    const Tgi& tgi,
    std::span<const RuntimePropertyRecord> source,
    std::size_t expectedCount,
    const RuntimeFloatPatch& patch,
    PatchedCacheStatus& status,
    std::string& error) noexcept {
    try {
        const std::scoped_lock lock(mutex_);
        const auto existing = copies_.find(tgi);
        if (existing != copies_.end()) {
            if (!MatchesCachedSource(source, existing->second.records(), patch)) {
                error = "Cached runtime resource does not match the repeated source";
                return nullptr;
            }
            status = PatchedCacheStatus::Reused;
            return &existing->second;
        }

        RuntimeResourceCopy copy;
        if (!RuntimeResourceCopy::Create(tgi, source, expectedCount, copy, error)) {
            return nullptr;
        }
        const RuntimeFloatPatchStatus patchStatus = copy.PatchFloat(patch);
        if (patchStatus != RuntimeFloatPatchStatus::Applied &&
            patchStatus != RuntimeFloatPatchStatus::AlreadyApplied) {
            error = "Owned runtime resource copy refused the requested float patch";
            return nullptr;
        }
        const auto [inserted, didInsert] = copies_.emplace(tgi, std::move(copy));
        if (!didInsert) {
            error = "Patched runtime resource cache insertion unexpectedly collided";
            return nullptr;
        }
        status = PatchedCacheStatus::Created;
        return &inserted->second;
    } catch (...) {
        error = "Patched runtime resource cache allocation failed";
        return nullptr;
    }
}

std::size_t PatchedResourceCache::size() const {
    const std::scoped_lock lock(mutex_);
    return copies_.size();
}

void PatchedResourceCache::Clear() noexcept {
    const std::scoped_lock lock(mutex_);
    copies_.clear();
}

}  // namespace sc13::core
