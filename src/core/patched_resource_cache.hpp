#pragma once

#include "core/runtime_property_table.hpp"
#include "core/tgi.hpp"

#include <cstddef>
#include <cstdint>
#include <map>
#include <mutex>
#include <span>
#include <string>
#include <vector>

namespace sc13::core {

/** Defines one exact scalar-float replacement applied to an owned runtime copy. */
struct RuntimeFloatPatch final {
    std::uint32_t identifier{};
    float expectedValue{};
    float replacementValue{};
};

/** Owns a validated runtime resource snapshot independently of game-managed memory. */
class RuntimeResourceCopy final {
public:
    /** Creates and validates an owned copy of one parsed runtime resource. */
    [[nodiscard]] static bool Create(
        const Tgi& tgi,
        std::span<const RuntimePropertyRecord> source,
        std::size_t expectedCount,
        RuntimeResourceCopy& copy,
        std::string& error) noexcept;

    /** Applies one exact float patch to the owned records. */
    [[nodiscard]] RuntimeFloatPatchStatus PatchFloat(
        const RuntimeFloatPatch& patch) noexcept;

    /** Returns the copied logical resource identity. */
    [[nodiscard]] const Tgi& tgi() const noexcept { return tgi_; }

    /** Returns immutable records whose lifetime is owned by this object. */
    [[nodiscard]] std::span<const RuntimePropertyRecord> records() const noexcept {
        return records_;
    }

private:
    Tgi tgi_{};
    std::vector<RuntimePropertyRecord> records_;
};

/** Reports whether a patched resource copy was created or safely reused. */
enum class PatchedCacheStatus {
    Created,
    Reused
};

/** Retains validated patched copies for stable process-lifetime ownership. */
class PatchedResourceCache final {
public:
    /** Returns a stable patched copy, refusing source drift for an existing TGI. */
    [[nodiscard]] const RuntimeResourceCopy* GetOrCreate(
        const Tgi& tgi,
        std::span<const RuntimePropertyRecord> source,
        std::size_t expectedCount,
        const RuntimeFloatPatch& patch,
        PatchedCacheStatus& status,
        std::string& error) noexcept;

    /** Returns the number of independently owned resource copies. */
    [[nodiscard]] std::size_t size() const;

    /** Releases every owned copy after runtime patch sites have been restored. */
    void Clear() noexcept;

private:
    mutable std::mutex mutex_;
    std::map<Tgi, RuntimeResourceCopy> copies_;
};

}  // namespace sc13::core
