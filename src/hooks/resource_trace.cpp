#include "hooks/resource_trace.hpp"

#include "core/runtime_property_table.hpp"
#include "core/tgi.hpp"
#include "logging/async_logger.hpp"
#include "mods/patch_registry.hpp"
#include "reverse/resource_symbols.hpp"
#include "runtime/patch_signal.hpp"
#include "runtime/runtime_resource_cache.hpp"

#include <Windows.h>
#include <MinHook.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace sc13::hooks {
namespace {

using PFRecordReadConstructor = void*(__thiscall*)(
    void*, void*, const std::uint32_t*, void*);
using PublishParsedResource = void(__cdecl*)(void*, void*, std::uint32_t);
using PropDeserialize = bool(__thiscall*)(void*, void*);

constexpr std::size_t kMaximumPropertyCount = 4096U;

/** Mirrors the verified x86 prefix produced by the game's PROP deserializer. */
struct ParsedResourcePrefix final {
    std::uint32_t vtable{};
    std::uint32_t unknown{};
    std::uint32_t instance{};
    std::uint32_t type{};
    std::uint32_t group{};
    std::uint32_t reserved{};
    std::uint32_t propertyBegin{};
    std::uint32_t propertyEnd{};
    std::uint32_t propertyCapacity{};
};

static_assert(sizeof(ParsedResourcePrefix) == 36U);

PFRecordReadConstructor g_originalPFRecordReadConstructor = nullptr;
PublishParsedResource g_originalPublishParsedResource = nullptr;
PropDeserialize g_originalPropDeserialize = nullptr;
mods::PatchRegistry* g_registry = nullptr;
runtime::RuntimeResourceCache* g_cache = nullptr;
SRWLOCK g_writeLock = SRWLOCK_INIT;

/** Releases one exclusive SRW lock on every exit path. */
class ExclusiveSrwLock final {
public:
    /** Acquires the supplied lock exclusively. */
    explicit ExclusiveSrwLock(SRWLOCK& lock) noexcept : lock_(lock) {
        AcquireSRWLockExclusive(&lock_);
    }

    /** Releases the lock after the guarded operation. */
    ~ExclusiveSrwLock() { ReleaseSRWLockExclusive(&lock_); }

    ExclusiveSrwLock(const ExclusiveSrwLock&) = delete;
    ExclusiveSrwLock& operator=(const ExclusiveSrwLock&) = delete;

private:
    SRWLOCK& lock_;
};

/** Copies a current-process address range without dereferencing an unchecked pointer. */
[[nodiscard]] bool ReadCurrentProcess(
    std::uintptr_t address,
    void* destination,
    std::size_t size) noexcept {
    SIZE_T bytesRead = 0U;
    return address != 0U && destination != nullptr && size != 0U &&
           ReadProcessMemory(
               GetCurrentProcess(), reinterpret_cast<const void*>(address), destination,
               size, &bytesRead) != FALSE &&
           bytesRead == size;
}

/** Atomically replaces one aligned writable dword only when its current bits match. */
[[nodiscard]] bool CompareExchangeDword(
    std::uintptr_t address,
    std::uint32_t expectedBits,
    std::uint32_t replacementBits,
    std::uint32_t& observedBits) noexcept {
    if (address == 0U || (address & (alignof(std::uint32_t) - 1U)) != 0U) {
        return false;
    }
    MEMORY_BASIC_INFORMATION information{};
    if (VirtualQuery(
            reinterpret_cast<const void*>(address), &information,
            sizeof(information)) != sizeof(information) ||
        information.State != MEM_COMMIT ||
        (information.Protect & (PAGE_GUARD | PAGE_NOACCESS)) != 0U) {
        return false;
    }
    const DWORD protection = information.Protect & 0xFFU;
    const bool writable = protection == PAGE_READWRITE ||
                          protection == PAGE_WRITECOPY ||
                          protection == PAGE_EXECUTE_READWRITE ||
                          protection == PAGE_EXECUTE_WRITECOPY;
    if (!writable) {
        return false;
    }
    auto* const value = reinterpret_cast<volatile LONG*>(address);
    observedBits = static_cast<std::uint32_t>(InterlockedCompareExchange(
        value, static_cast<LONG>(replacementBits), static_cast<LONG>(expectedBits)));
    return true;
}

/** Compares complete property records without depending on structure padding. */
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

/** Validates one game-owned PROP vector and copies it into loader-owned storage. */
[[nodiscard]] bool ReadPropertyTable(
    const ParsedResourcePrefix& prefix,
    std::vector<core::RuntimePropertyRecord>& properties,
    std::string& error) noexcept {
    try {
        properties.clear();
        if (prefix.reserved != 0U || prefix.propertyBegin == 0U ||
            prefix.propertyEnd < prefix.propertyBegin ||
            prefix.propertyEnd != prefix.propertyCapacity ||
            (prefix.propertyBegin &
             (alignof(core::RuntimePropertyRecord) - 1U)) != 0U) {
            error = "PROP vector bounds or alignment are invalid";
            return false;
        }
        const std::size_t byteSize =
            static_cast<std::size_t>(prefix.propertyEnd - prefix.propertyBegin);
        if (byteSize == 0U ||
            byteSize % sizeof(core::RuntimePropertyRecord) != 0U) {
            error = "PROP vector byte length is not a non-empty record sequence";
            return false;
        }
        const std::size_t count = byteSize / sizeof(core::RuntimePropertyRecord);
        if (count > kMaximumPropertyCount) {
            error = "PROP vector exceeds the runtime safety limit";
            return false;
        }
        properties.resize(count);
        if (!ReadCurrentProcess(prefix.propertyBegin, properties.data(), byteSize)) {
            properties.clear();
            error = "PROP vector could not be copied from game memory";
            return false;
        }
        if (core::ValidateRuntimePropertyTable(properties, count) !=
            core::RuntimeTableStatus::Valid) {
            properties.clear();
            error = "PROP vector structure is invalid";
            return false;
        }
        return true;
    } catch (...) {
        properties.clear();
        error = "PROP vector copy failed due to an allocation exception";
        return false;
    }
}

/** Applies only value-bit differences and rolls all earlier writes back on failure. */
[[nodiscard]] bool ApplyValueTransition(
    const ParsedResourcePrefix& prefix,
    std::span<const core::RuntimePropertyRecord> from,
    std::span<const core::RuntimePropertyRecord> to,
    std::size_t& changedCount) noexcept {
    changedCount = 0U;
    if (from.size() != to.size()) {
        return false;
    }
    std::vector<std::size_t> written;
    try {
        written.reserve(from.size());
        for (std::size_t index = 0U; index < from.size(); ++index) {
            if (from[index].id != to[index].id ||
                from[index].auxiliary != to[index].auxiliary ||
                from[index].metadata != to[index].metadata) {
                return false;
            }
            if (from[index].valueBits == to[index].valueBits) {
                continue;
            }
            const std::uintptr_t address =
                prefix.propertyBegin +
                index * sizeof(core::RuntimePropertyRecord) +
                offsetof(core::RuntimePropertyRecord, valueBits);
            std::uint32_t observed = 0U;
            if (!CompareExchangeDword(
                    address, from[index].valueBits, to[index].valueBits, observed) ||
                observed != from[index].valueBits) {
                for (auto rollback = written.rbegin(); rollback != written.rend(); ++rollback) {
                    const std::uintptr_t rollbackAddress =
                        prefix.propertyBegin +
                        *rollback * sizeof(core::RuntimePropertyRecord) +
                        offsetof(core::RuntimePropertyRecord, valueBits);
                    std::uint32_t rollbackObserved = 0U;
                    const bool rolledBack = CompareExchangeDword(
                        rollbackAddress, to[*rollback].valueBits,
                        from[*rollback].valueBits, rollbackObserved);
                    if (!rolledBack || rollbackObserved != to[*rollback].valueBits) {
                        logging::AsyncLogger::Instance().WriteFormat(
                            logging::Level::Error,
                            "[SC13][PATCH] transactional rollback refused at address=%p",
                            reinterpret_cast<void*>(rollbackAddress));
                    }
                }
                return false;
            }
            written.push_back(index);
        }
        changedCount = written.size();
        return true;
    } catch (...) {
        for (auto rollback = written.rbegin(); rollback != written.rend(); ++rollback) {
            const std::uintptr_t rollbackAddress =
                prefix.propertyBegin +
                *rollback * sizeof(core::RuntimePropertyRecord) +
                offsetof(core::RuntimePropertyRecord, valueBits);
            std::uint32_t observed = 0U;
            const bool rolledBack = CompareExchangeDword(
                rollbackAddress, to[*rollback].valueBits,
                from[*rollback].valueBits, observed);
            static_cast<void>(rolledBack);
        }
        return false;
    }
}

/** Writes and commits one prepared resource generation with transactional rollback. */
[[nodiscard]] bool ApplyAndCommitPlan(
    const ParsedResourcePrefix& prefix,
    const runtime::RuntimeResourceBuildPlan& plan,
    std::size_t& changedCount,
    std::string& error) noexcept {
    bool written = false;
    bool committed = false;
    {
        const ExclusiveSrwLock writeLock(g_writeLock);
        written = ApplyValueTransition(
            prefix, plan.previous, plan.desired, changedCount);
        committed = written && g_cache != nullptr && g_cache->Commit(plan, error);
        if (!committed && written && changedCount != 0U) {
            std::size_t rollbackCount = 0U;
            const bool rolledBack = ApplyValueTransition(
                prefix, plan.desired, plan.previous, rollbackCount);
            if (!rolledBack) {
                logging::AsyncLogger::Instance().Write(
                    logging::Level::Error,
                    "[SC13][PATCH] cache commit race rollback was incomplete");
            }
        }
    }
    if (!written && error.empty()) {
        error = "game memory changed concurrently";
    }
    return written && committed;
}

/** @summary Logs semantic identities and committed resource values without attributing the final value to one mod. */
void LogResolvedPatches(
    std::span<const mods::RegisteredPatch> patches,
    const runtime::RuntimeResourceBuildPlan& plan) {
    if (plan.status == runtime::RuntimeResourceBuildStatus::Reused) return;
    for (const auto& registered : patches) {
        for (const auto& property : registered.patch.properties) {
            const auto vanilla = core::ReadRuntimeFloat(plan.vanilla, property.propertyId);
            const auto finalValue = core::ReadRuntimeFloat(plan.desired, property.propertyId);
            if (!vanilla || !finalValue) continue;
            logging::AsyncLogger::Instance().WriteEventFormat(
                logging::Level::Patch, logging::SourceType::Mod, registered.owner.c_str(),
                "Resolved resource=%s TGI=%s property=%s id=0x%08X operation=%s operand=%.9g "
                "vanilla=%.9g resourceFinal=%.9g source=%s resourceSource=%s",
                registered.patch.name.c_str(), core::ToString(plan.key.target).c_str(),
                property.name.c_str(), property.propertyId, mods::PatchOperationName(property.operation),
                property.value, *vanilla, *finalValue, property.source.c_str(), registered.patch.source.c_str());
        }
    }
}

/** Rebuilds one matched resource from retained vanilla and the current registry snapshot. */
void PatchDeserializedResource(
    void* resource,
    void* reader,
    bool deserializeResult) noexcept {
    try {
        if (resource == nullptr || !deserializeResult ||
            g_registry == nullptr || g_cache == nullptr) {
            return;
        }
        ParsedResourcePrefix prefix{};
        if (!ReadCurrentProcess(
                reinterpret_cast<std::uintptr_t>(resource), &prefix, sizeof(prefix))) {
            return;
        }
        const core::Tgi target{prefix.type, prefix.group, prefix.instance};
        const runtime::RuntimeResourceKey key{
            target, reinterpret_cast<std::uintptr_t>(resource)};
        const std::shared_ptr<const mods::PatchRegistrySnapshot> snapshot =
            g_registry->Current();
        const std::vector<mods::RegisteredPatch>* const registered =
            snapshot->Find(target);
        if (registered == nullptr && !g_cache->Contains(key)) {
            return;
        }

        std::vector<core::RuntimePropertyRecord> current;
        std::string error;
        if (!ReadPropertyTable(prefix, current, error)) {
            g_cache->RecordFailure(key, snapshot->generation(), error);
            logging::AsyncLogger::Instance().WriteFormat(
                logging::Level::Error,
                "[SC13][PATCH] TGI=%s resource=%p rejected: %s; fail-open",
                core::ToString(target).c_str(), resource, error.c_str());
            return;
        }
        const std::span<const mods::RegisteredPatch> patches =
            registered == nullptr ? std::span<const mods::RegisteredPatch>{}
                                  : std::span<const mods::RegisteredPatch>(*registered);
        runtime::RuntimeResourceBuildPlan plan;
        if (!g_cache->Prepare(
                key, current, snapshot->generation(), patches, plan, error)) {
            g_cache->RecordFailure(key, snapshot->generation(), error);
            logging::AsyncLogger::Instance().WriteFormat(
                logging::Level::Error,
                "[SC13][PATCH] TGI=%s resource=%p rebuild rejected: %s; fail-open",
                core::ToString(target).c_str(), resource, error.c_str());
            return;
        }

        std::size_t changedCount = 0U;
        if (!ApplyAndCommitPlan(prefix, plan, changedCount, error)) {
            g_cache->RecordFailure(key, snapshot->generation(), error.empty() ? "Concurrent memory change" : error);
            logging::AsyncLogger::Instance().WriteFormat(
                logging::Level::Error,
                "[SC13][PATCH] TGI=%s resource=%p transactional write refused: %s; fail-open",
                core::ToString(target).c_str(), resource,
                error.empty() ? "game memory changed concurrently" : error.c_str());
            return;
        }
        logging::AsyncLogger::Instance().WriteFormat(
            logging::Level::Patch,
            "[SC13][PATCH] TGI=%s resource=%p reader=%p generation=%llu "
            "patchOwners=%zu changedValues=%zu cache=%s",
            core::ToString(target).c_str(), resource, reader,
            static_cast<unsigned long long>(snapshot->generation()), patches.size(),
            changedCount, runtime::RuntimeResourceBuildStatusName(plan.status));
        LogResolvedPatches(patches, plan);
        for (const mods::RegisteredPatch& patch : patches) {
            logging::AsyncLogger::Instance().WriteEventFormat(
                logging::Level::Patch,
                logging::SourceType::Mod,
                patch.owner.c_str(),
                "Applied resource TGI=%s generation=%llu changedValues=%zu",
                core::ToString(target).c_str(),
                static_cast<unsigned long long>(snapshot->generation()),
                changedCount);
        }
        if (changedCount != 0U && !runtime::SignalPatchApplied()) {
            logging::AsyncLogger::Instance().Write(
                logging::Level::Warning,
                "[SC13][PATCH] Runtime write succeeded but patch-applied signal failed");
        }
    } catch (...) {
        logging::AsyncLogger::Instance().Write(
            logging::Level::Error,
            "[SC13][PATCH] Resource hook caught an exception and failed open");
    }
}

/** Intercepts the verified PROP deserializer after its game-owned table is complete. */
bool __fastcall HookPropDeserialize(
    void* self,
    void*,
    void* reader) noexcept {
    const bool result = g_originalPropDeserialize(self, reader);
    PatchDeserializedResource(self, reader, result);
    return result;
}

/** Restores every retained live resource when its identity and last result still match. */
void RestoreRuntimeResources() noexcept {
    if (g_cache == nullptr) {
        return;
    }
    try {
        const std::vector<runtime::RuntimeResourceRestoreEntry> entries =
            g_cache->SnapshotForRestore();
        const ExclusiveSrwLock writeLock(g_writeLock);
        for (const runtime::RuntimeResourceRestoreEntry& entry : entries) {
            ParsedResourcePrefix prefix{};
            std::vector<core::RuntimePropertyRecord> current;
            std::string error;
            const bool identityValid = ReadCurrentProcess(
                entry.key.resourceAddress, &prefix, sizeof(prefix)) &&
                core::Tgi{prefix.type, prefix.group, prefix.instance} == entry.key.target;
            const bool tableValid =
                identityValid && ReadPropertyTable(prefix, current, error);
            if (!tableValid || EqualRecords(current, entry.vanilla)) {
                continue;
            }
            if (!EqualRecords(current, entry.lastApplied)) {
                logging::AsyncLogger::Instance().WriteFormat(
                    logging::Level::Warning,
                    "[SC13][PATCH] detach restore skipped changed resource=%p TGI=%s",
                    reinterpret_cast<void*>(entry.key.resourceAddress),
                    core::ToString(entry.key.target).c_str());
                continue;
            }
            std::size_t restoredCount = 0U;
            const bool restored = ApplyValueTransition(
                prefix, entry.lastApplied, entry.vanilla, restoredCount);
            logging::AsyncLogger::Instance().WriteFormat(
                restored ? logging::Level::Info : logging::Level::Warning,
                "[SC13][PATCH] detach restore resource=%p TGI=%s values=%zu status=%s",
                reinterpret_cast<void*>(entry.key.resourceAddress),
                core::ToString(entry.key.target).c_str(), restoredCount,
                restored ? "restored" : "skipped");
        }
    } catch (...) {
        logging::AsyncLogger::Instance().Write(
            logging::Level::Error,
            "[SC13][PATCH] detach restoration failed safely with an exception");
    }
    g_cache->Clear();
}

#if defined(SC13_ENABLE_DISCOVERY_TRACE)
/** Reports whether a TGI is currently relevant to any active declarative patch. */
[[nodiscard]] bool HasActivePatch(const core::Tgi& target) noexcept {
    return g_registry != nullptr && g_registry->Current()->Find(target) != nullptr;
}

/** Observes construction of reader records for currently active resource identities. */
void* __fastcall HookPFRecordReadConstructor(
    void* self,
    void*,
    void* ownerContext,
    const std::uint32_t* keyArgument,
    void* options) noexcept {
    void* const result =
        g_originalPFRecordReadConstructor(self, ownerContext, keyArgument, options);
    if (self != nullptr) {
        std::array<std::uint32_t, 3> words{};
        if (ReadCurrentProcess(
                reinterpret_cast<std::uintptr_t>(self) + 0x0CU,
                words.data(), sizeof(words))) {
            const core::Tgi target{words[1], words[2], words[0]};
            if (HasActivePatch(target)) {
                logging::AsyncLogger::Instance().WriteFormat(
                    logging::Level::Trace,
                    "[SC13][RESOURCE] TGI=%s PFRecordRead=%p owner=%p key=%p options=%p",
                    core::ToString(target).c_str(), self, ownerContext, keyArgument, options);
            }
        }
    }
    return result;
}

/** Observes publication of resources currently targeted by active mods. */
void __cdecl HookPublishParsedResource(
    void* resource,
    void* context,
    std::uint32_t notify) noexcept {
    if (resource != nullptr) {
        ParsedResourcePrefix prefix{};
        if (ReadCurrentProcess(
                reinterpret_cast<std::uintptr_t>(resource), &prefix, sizeof(prefix))) {
            const core::Tgi target{prefix.type, prefix.group, prefix.instance};
            if (HasActivePatch(target)) {
                logging::AsyncLogger::Instance().WriteFormat(
                    logging::Level::Trace,
                    "[SC13][RESOURCE] TGI=%s publish resource=%p context=%p notify=%u",
                    core::ToString(target).c_str(), resource, context, notify);
            }
        }
    }
    g_originalPublishParsedResource(resource, context, notify);
}
#endif

}  // namespace

void ConfigureResourceRuntime(
    mods::PatchRegistry& registry,
    runtime::RuntimeResourceCache& cache) noexcept {
    g_registry = &registry;
    g_cache = &cache;
}

bool RefreshRuntimeResources(
    const std::set<core::Tgi>& affected,
    std::string& error) noexcept {
    try {
        error.clear();
        if (g_registry == nullptr || g_cache == nullptr) {
            error = "Runtime patch services are not configured";
            return false;
        }
        if (affected.empty()) {
            return true;
        }

        const std::shared_ptr<const mods::PatchRegistrySnapshot> snapshot =
            g_registry->Current();
        const std::vector<runtime::RuntimeResourceRestoreEntry> entries =
            g_cache->SnapshotForRestore();
        std::size_t refreshedResources = 0U;
        std::size_t changedValues = 0U;
        std::size_t staleResources = 0U;
        std::size_t failedResources = 0U;
        for (const runtime::RuntimeResourceRestoreEntry& entry : entries) {
            if (!affected.contains(entry.key.target)) {
                continue;
            }
            ParsedResourcePrefix prefix{};
            const bool identityValid = ReadCurrentProcess(
                entry.key.resourceAddress, &prefix, sizeof(prefix)) &&
                core::Tgi{prefix.type, prefix.group, prefix.instance} == entry.key.target;
            std::vector<core::RuntimePropertyRecord> current;
            std::string resourceError;
            if (!identityValid || !ReadPropertyTable(prefix, current, resourceError)) {
                g_cache->RecordFailure(entry.key, snapshot->generation(), "Resource identity or layout is stale");
                ++staleResources;
                continue;
            }

            const std::vector<mods::RegisteredPatch>* const registered =
                snapshot->Find(entry.key.target);
            const std::span<const mods::RegisteredPatch> patches =
                registered == nullptr ? std::span<const mods::RegisteredPatch>{}
                                      : std::span<const mods::RegisteredPatch>(*registered);
            runtime::RuntimeResourceBuildPlan plan;
            if (!g_cache->Prepare(
                    entry.key, current, snapshot->generation(), patches,
                    plan, resourceError)) {
                g_cache->RecordFailure(entry.key, snapshot->generation(), resourceError);
                ++failedResources;
                logging::AsyncLogger::Instance().WriteFormat(
                    logging::Level::Warning,
                    "[SC13][PATCH] live refresh prepare rejected resource=%p TGI=%s: %s",
                    reinterpret_cast<void*>(entry.key.resourceAddress),
                    core::ToString(entry.key.target).c_str(), resourceError.c_str());
                continue;
            }
            std::size_t resourceChangedValues = 0U;
            if (!ApplyAndCommitPlan(
                    prefix, plan, resourceChangedValues, resourceError)) {
                g_cache->RecordFailure(entry.key, snapshot->generation(), resourceError.empty() ? "Concurrent memory change" : resourceError);
                ++failedResources;
                logging::AsyncLogger::Instance().WriteFormat(
                    logging::Level::Warning,
                    "[SC13][PATCH] live refresh write rejected resource=%p TGI=%s: %s",
                    reinterpret_cast<void*>(entry.key.resourceAddress),
                    core::ToString(entry.key.target).c_str(), resourceError.c_str());
                continue;
            }
            ++refreshedResources;
            LogResolvedPatches(patches, plan);
            changedValues += resourceChangedValues;
            logging::AsyncLogger::Instance().WriteFormat(
                logging::Level::Patch,
                "[SC13][PATCH] live refresh resource=%p TGI=%s generation=%llu "
                "patchOwners=%zu changedValues=%zu cache=%s",
                reinterpret_cast<void*>(entry.key.resourceAddress),
                core::ToString(entry.key.target).c_str(),
                static_cast<unsigned long long>(snapshot->generation()), patches.size(),
                resourceChangedValues,
                runtime::RuntimeResourceBuildStatusName(plan.status));
        }
        logging::AsyncLogger::Instance().WriteFormat(
            failedResources == 0U ? logging::Level::Info : logging::Level::Warning,
            "[SC13][PATCH] live refresh summary generation=%llu affected=%zu "
            "refreshed=%zu stale=%zu failed=%zu changedValues=%zu",
            static_cast<unsigned long long>(snapshot->generation()), affected.size(),
            refreshedResources, staleResources, failedResources, changedValues);
        if (changedValues != 0U && !runtime::SignalPatchApplied()) {
            logging::AsyncLogger::Instance().Write(
                logging::Level::Warning,
                "[SC13][PATCH] Live refresh succeeded but patch-applied signal failed");
        }
        if (failedResources != 0U) {
            error = std::to_string(failedResources) +
                " retained live resource(s) could not be rebuilt";
            return false;
        }
        return true;
    } catch (...) {
        error = "Live resource refresh failed safely with an exception";
        return false;
    }
}

bool CreateResourceTraceHook(
    const reverse::BuildFingerprint& fingerprint,
    std::string& error) noexcept {
    if (g_registry == nullptr || g_cache == nullptr) {
        error = "Resource runtime services were not configured";
        return false;
    }
#if defined(SC13_ENABLE_DISCOVERY_TRACE)
    reverse::ResourceSymbols symbols{};
    if (!reverse::ResolveResourceSymbols(fingerprint, symbols, error)) {
        return false;
    }
    const MH_STATUS readerStatus = MH_CreateHook(
        symbols.pfRecordReadConstructor,
        reinterpret_cast<LPVOID>(&HookPFRecordReadConstructor),
        reinterpret_cast<LPVOID*>(&g_originalPFRecordReadConstructor));
    if (readerStatus != MH_OK) {
        error = std::string("MH_CreateHook failed for PFRecordRead: ") +
                MH_StatusToString(readerStatus);
        return false;
    }
    const MH_STATUS publishStatus = MH_CreateHook(
        symbols.publishParsedResource,
        reinterpret_cast<LPVOID>(&HookPublishParsedResource),
        reinterpret_cast<LPVOID*>(&g_originalPublishParsedResource));
    if (publishStatus != MH_OK) {
        error = std::string("MH_CreateHook failed for parsed-resource publisher: ") +
                MH_StatusToString(publishStatus);
        return false;
    }
#else
    reverse::PropDeserializeSymbol propDeserialize{};
    if (!reverse::ResolvePropDeserializeSymbol(fingerprint, propDeserialize, error)) {
        return false;
    }
#endif
    const MH_STATUS deserializeStatus = MH_CreateHook(
#if defined(SC13_ENABLE_DISCOVERY_TRACE)
        symbols.propDeserialize,
#else
        propDeserialize.address,
#endif
        reinterpret_cast<LPVOID>(&HookPropDeserialize),
        reinterpret_cast<LPVOID*>(&g_originalPropDeserialize));
    if (deserializeStatus != MH_OK) {
        error = std::string("MH_CreateHook failed for PROP deserializer: ") +
                MH_StatusToString(deserializeStatus);
        return false;
    }
    logging::AsyncLogger::Instance().WriteFormat(
        logging::Level::Info,
        "Resolved PROP deserializer uniquely: module=SimCity.exe RVA=0x%08X address=%p",
#if defined(SC13_ENABLE_DISCOVERY_TRACE)
        symbols.propDeserializeRva, symbols.propDeserialize);
#else
        propDeserialize.rva, propDeserialize.address);
#endif
    return true;
}

void ResetResourceTrace() noexcept {
    RestoreRuntimeResources();
    g_originalPFRecordReadConstructor = nullptr;
    g_originalPublishParsedResource = nullptr;
    g_originalPropDeserialize = nullptr;
    g_registry = nullptr;
    g_cache = nullptr;
}

}  // namespace sc13::hooks
