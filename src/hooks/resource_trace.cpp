#include "hooks/resource_trace.hpp"

#include "core/patched_resource_cache.hpp"
#include "core/resource_filter.hpp"
#include "core/runtime_property_table.hpp"
#include "core/tgi.hpp"
#include "logging/async_logger.hpp"
#include "reverse/resource_symbols.hpp"
#include "runtime/patch_signal.hpp"

#include <Windows.h>
#include <MinHook.h>

#include <array>
#include <atomic>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>

namespace sc13::hooks {
namespace {

using PFRecordReadConstructor = void*(__thiscall*)(
    void*, void*, const std::uint32_t*, void*);
using PublishParsedResource = void(__cdecl*)(void*, void*, std::uint32_t);
using PropDeserialize = bool(__thiscall*)(void*, void*);

constexpr std::uint32_t kTargetType = 0x00B1B104U;
constexpr std::uint32_t kTargetGroup = 0x61EFC000U;
constexpr std::uint32_t kTargetInstance = 0x719436BDU;
constexpr core::ResourceFilter kTargetFilter{
    kTargetType, kTargetGroup, kTargetInstance};
constexpr std::size_t kExpectedPropertyCount = 89U;
constexpr std::array<std::uint32_t, 4> kObservedProperties{
    0x09AE19D7U, 0x0AFB9882U, 0x0C09DA83U, 0x0FD16C15U};
constexpr std::array<float, 4> kExpectedPropertyValues{300.0F, 72.0F, 2.0F, 200.0F};
constexpr core::RuntimeFloatPatch kMaintenancePatch{0x09AE19D7U, 300.0F, 345.0F};
constexpr std::size_t kTrackedPatchCapacity = 8U;

/** Mirrors the verified x86 prefix passed to the parsed-resource publisher. */
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

/** Retains enough identity to restore one exact game-managed float safely. */
struct AppliedRuntimePatch final {
    std::uintptr_t resourceAddress{};
    std::uintptr_t tableBegin{};
    std::uintptr_t valueAddress{};
    std::uint32_t originalBits{};
    std::uint32_t replacementBits{};
};

PFRecordReadConstructor g_originalPFRecordReadConstructor = nullptr;
PublishParsedResource g_originalPublishParsedResource = nullptr;
PropDeserialize g_originalPropDeserialize = nullptr;
SRWLOCK g_patchLock = SRWLOCK_INIT;
std::array<AppliedRuntimePatch, kTrackedPatchCapacity> g_appliedPatches{};
core::PatchedResourceCache g_patchedResourceCache;

/** Copies a current-process address range without directly dereferencing untrusted pointers. */
[[nodiscard]] bool ReadCurrentProcess(
    std::uintptr_t address,
    void* destination,
    std::size_t size) noexcept {
    SIZE_T bytesRead = 0;
    return address != 0U &&
           ReadProcessMemory(
               GetCurrentProcess(), reinterpret_cast<const void*>(address), destination, size,
               &bytesRead) != FALSE &&
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
    const DWORD baseProtection = information.Protect & 0xFFU;
    const bool writable = baseProtection == PAGE_READWRITE ||
                          baseProtection == PAGE_WRITECOPY ||
                          baseProtection == PAGE_EXECUTE_READWRITE ||
                          baseProtection == PAGE_EXECUTE_WRITECOPY;
    if (!writable) {
        return false;
    }
    auto* const value = reinterpret_cast<volatile LONG*>(address);
    observedBits = static_cast<std::uint32_t>(InterlockedCompareExchange(
        value, static_cast<LONG>(replacementBits), static_cast<LONG>(expectedBits)));
    return true;
}

/** Logs a bounded module-relative call stack for a matched target resource key. */
void LogTargetStack() noexcept {
    std::array<void*, 16> frames{};
    const USHORT captured = CaptureStackBackTrace(
        2, static_cast<DWORD>(frames.size()), frames.data(), nullptr);
    for (USHORT index = 0; index < captured; ++index) {
        HMODULE module = nullptr;
        if (GetModuleHandleExW(
                GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                    GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                reinterpret_cast<LPCWSTR>(frames[index]), &module) == FALSE) {
            logging::AsyncLogger::Instance().WriteFormat(
                logging::Level::Trace,
                "  target-stack[%u] address=%p module=unknown", index, frames[index]);
            continue;
        }
        std::array<wchar_t, 1024> modulePath{};
        GetModuleFileNameW(
            module, modulePath.data(), static_cast<DWORD>(modulePath.size()));
        const auto rva = reinterpret_cast<std::uintptr_t>(frames[index]) -
                         reinterpret_cast<std::uintptr_t>(module);
        logging::AsyncLogger::Instance().WriteFormat(
            logging::Level::Trace, "  target-stack[%u] %ls+0x%08llX", index,
            modulePath.data(), static_cast<unsigned long long>(rva));
    }
}

/** Records and applies one exact maintenance float write to game-managed memory. */
[[nodiscard]] bool ApplyMaintenancePatch(
    void* resource,
    const ParsedResourcePrefix& prefix,
    std::span<const core::RuntimePropertyRecord> original,
    const core::RuntimeResourceCopy& patchedCopy,
    core::PatchedCacheStatus cacheStatus) noexcept {
    const core::RuntimePropertyRecord* const originalProperty =
        core::FindRuntimeProperty(original, kMaintenancePatch.identifier);
    const core::RuntimePropertyRecord* const patchedProperty =
        core::FindRuntimeProperty(patchedCopy.records(), kMaintenancePatch.identifier);
    if (originalProperty == nullptr || patchedProperty == nullptr) {
        return false;
    }
    const std::size_t recordIndex =
        static_cast<std::size_t>(originalProperty - original.data());
    const std::uintptr_t valueAddress =
        prefix.propertyBegin + recordIndex * sizeof(core::RuntimePropertyRecord) +
        offsetof(core::RuntimePropertyRecord, valueBits);
    const std::uint32_t expectedBits =
        std::bit_cast<std::uint32_t>(kMaintenancePatch.expectedValue);
    const std::uint32_t replacementBits = patchedProperty->valueBits;

    AcquireSRWLockExclusive(&g_patchLock);
    AppliedRuntimePatch* available = nullptr;
    for (AppliedRuntimePatch& site : g_appliedPatches) {
        if (site.valueAddress == valueAddress) {
            available = &site;
            break;
        }
        if (available == nullptr && site.valueAddress == 0U) {
            available = &site;
        }
    }
    std::uint32_t observedBits = 0U;
    const bool exchanged = available != nullptr && CompareExchangeDword(
        valueAddress, expectedBits, replacementBits, observedBits);
    const bool written = exchanged &&
        (observedBits == expectedBits ||
         (available->valueAddress == valueAddress && observedBits == replacementBits));
    if (written) {
        *available = AppliedRuntimePatch{
            reinterpret_cast<std::uintptr_t>(resource), prefix.propertyBegin, valueAddress,
            expectedBits, replacementBits};
    }
    ReleaseSRWLockExclusive(&g_patchLock);

    if (written) {
        logging::AsyncLogger::Instance().WriteFormat(
            logging::Level::Info,
            "[SC13][PATCH] TGI=%s id=0x%08X old=%.3f new=%.3f address=%p "
            "bytes=4 cache=%s status=%s",
            core::ToString(patchedCopy.tgi()).c_str(), kMaintenancePatch.identifier,
            kMaintenancePatch.expectedValue, kMaintenancePatch.replacementValue,
            reinterpret_cast<void*>(valueAddress),
            cacheStatus == core::PatchedCacheStatus::Created ? "created" : "reused",
            observedBits == replacementBits ? "already-applied" : "applied");
        if (!runtime::SignalPatchApplied()) {
            logging::AsyncLogger::Instance().Write(
                logging::Level::Warning,
                "[SC13][PATCH] Runtime write succeeded but patch-applied signal failed");
        }
    }
    return written;
}

/** Validates and patches one target PROP table at its earliest verified boundary. */
void PatchDeserializedTarget(void* resource, void* reader, bool deserializeResult) noexcept {
    if (resource == nullptr) {
        return;
    }
    std::array<std::uint32_t, 3> identityWords{};
    std::memcpy(
        identityWords.data(), static_cast<const std::byte*>(resource) + 0x08U,
        sizeof(identityWords));
    const core::Tgi tgi{identityWords[1], identityWords[2], identityWords[0]};
    if (!kTargetFilter.Matches(tgi)) {
        return;
    }

    ParsedResourcePrefix prefix{};
    const bool prefixCopied = ReadCurrentProcess(
        reinterpret_cast<std::uintptr_t>(resource), &prefix, sizeof(prefix));
    std::array<core::RuntimePropertyRecord, kExpectedPropertyCount> properties{};
    const bool boundsValid = prefixCopied && prefix.reserved == 0U &&
                             prefix.propertyBegin != 0U &&
                             prefix.propertyEnd >= prefix.propertyBegin &&
                             prefix.propertyEnd - prefix.propertyBegin ==
                                 properties.size() * sizeof(properties.front()) &&
                              prefix.propertyCapacity == prefix.propertyEnd &&
                              (prefix.propertyBegin &
                               (alignof(core::RuntimePropertyRecord) - 1U)) == 0U;
    const bool copied = boundsValid && ReadCurrentProcess(
        prefix.propertyBegin, properties.data(), sizeof(properties));
    const bool tableValid = copied &&
        core::ValidateRuntimePropertyTable(properties, kExpectedPropertyCount) ==
            core::RuntimeTableStatus::Valid;
    std::array<float, kObservedProperties.size()> values{};
    bool valuesValid = tableValid;
    for (std::size_t index = 0; index < kObservedProperties.size() && valuesValid; ++index) {
        const auto value = core::ReadRuntimeFloat(properties, kObservedProperties[index]);
        valuesValid = value.has_value();
        if (value.has_value()) {
            values[index] = value.value();
        }
    }
    bool baselineValid = deserializeResult && valuesValid;
    for (std::size_t index = 0; index < values.size() && baselineValid; ++index) {
        baselineValid = values[index] == kExpectedPropertyValues[index];
    }

    logging::AsyncLogger::Instance().WriteFormat(
        baselineValid ? logging::Level::Info : logging::Level::Error,
        "[SC13][PROP-DESERIALIZE] thread=%lu TGI=%s result=%s resource=%p reader=%p "
        "bounds=%s table=%s baseline=%s mode=patch-candidate fail-open",
        GetCurrentThreadId(), core::ToString(tgi).c_str(),
        deserializeResult ? "success" : "failure", resource, reader,
        boundsValid ? "valid" : "invalid", tableValid ? "valid" : "invalid",
        baselineValid ? "verified" : "rejected");
    if (valuesValid) {
        for (std::size_t index = 0; index < kObservedProperties.size(); ++index) {
            logging::AsyncLogger::Instance().WriteFormat(
                logging::Level::Info, "[SC13][PROP-DESERIALIZE] id=0x%08X float=%.3f",
                kObservedProperties[index], values[index]);
        }
    }
    LogTargetStack();

    if (!baselineValid) {
        logging::AsyncLogger::Instance().Write(
            logging::Level::Warning,
            "[SC13][PATCH] Target baseline rejected; original runtime resource remains unchanged");
        return;
    }
    core::PatchedCacheStatus cacheStatus{};
    std::string cacheError;
    const core::RuntimeResourceCopy* const patchedCopy =
        g_patchedResourceCache.GetOrCreate(
            tgi, properties, kExpectedPropertyCount, kMaintenancePatch, cacheStatus,
            cacheError);
    if (patchedCopy == nullptr ||
        !ApplyMaintenancePatch(resource, prefix, properties, *patchedCopy, cacheStatus)) {
        logging::AsyncLogger::Instance().WriteFormat(
            logging::Level::Error,
            "[SC13][PATCH] Exact maintenance patch refused: %s; fail-open",
            cacheError.empty() ? "runtime write validation failed" : cacheError.c_str());
    }
}

/** Intercepts the concrete PROP deserializer and observes its completed target table. */
bool __fastcall HookPropDeserialize(
    void* self,
    void*,
    void* reader) noexcept {
    const bool result = g_originalPropDeserialize(self, reader);
    PatchDeserializedTarget(self, reader, result);
    return result;
}

/** Restores tracked target floats only while their resource identity and bytes still match. */
void RestoreRuntimePatches() noexcept {
    AcquireSRWLockExclusive(&g_patchLock);
    for (AppliedRuntimePatch& site : g_appliedPatches) {
        if (site.valueAddress == 0U) {
            continue;
        }
        ParsedResourcePrefix prefix{};
        std::uint32_t currentBits = 0U;
        const bool readable =
            ReadCurrentProcess(site.resourceAddress, &prefix, sizeof(prefix)) &&
            ReadCurrentProcess(site.valueAddress, &currentBits, sizeof(currentBits));
        const core::Tgi tgi{prefix.type, prefix.group, prefix.instance};
        const bool identityValid = readable && kTargetFilter.Matches(tgi) &&
                                   prefix.reserved == 0U &&
                                   prefix.propertyBegin == site.tableBegin &&
                                   prefix.propertyEnd == prefix.propertyCapacity &&
                                   site.valueAddress >= prefix.propertyBegin &&
                                   site.valueAddress + sizeof(currentBits) <= prefix.propertyEnd;
        std::uint32_t observedBits = 0U;
        const bool exchanged = identityValid && CompareExchangeDword(
            site.valueAddress, site.replacementBits, site.originalBits, observedBits);
        const bool restored = exchanged && observedBits == site.replacementBits;
        const bool alreadyOriginal = exchanged && observedBits == site.originalBits;
        logging::AsyncLogger::Instance().WriteFormat(
            restored || alreadyOriginal ? logging::Level::Info : logging::Level::Warning,
            "[SC13][PATCH] rollback address=%p status=%s",
            reinterpret_cast<void*>(site.valueAddress),
            restored ? "restored-300" : alreadyOriginal ? "already-300" : "skipped");
        site = {};
    }
    ReleaseSRWLockExclusive(&g_patchLock);
    g_patchedResourceCache.Clear();
}

/** Intercepts PFRecordRead construction and filters its verified second argument as instance/type/group. */
void* __fastcall HookPFRecordReadConstructor(
    void* self,
    void*,
    void* ownerContext,
    const std::uint32_t* keyArgument,
    void* options) noexcept {
    void* const result =
        g_originalPFRecordReadConstructor(self, ownerContext, keyArgument, options);

    std::array<std::uint32_t, 3> runtimeWords{};
    std::memcpy(
        runtimeWords.data(), static_cast<const std::byte*>(self) + 0x0CU,
        sizeof(runtimeWords));
    const core::Tgi tgi{runtimeWords[1], runtimeWords[2], runtimeWords[0]};
    if (kTargetFilter.Matches(tgi)) {
        logging::AsyncLogger::Instance().WriteFormat(
            logging::Level::Info,
            "[SC13][RESOURCE] thread=%lu TGI=%s PFRecordRead=%p owner=%p keyArg=%p "
            "options=%p runtime-order=instance/type/group",
            GetCurrentThreadId(), core::ToString(tgi).c_str(), self, ownerContext,
            keyArgument, options);
        logging::AsyncLogger::Instance().Write(
            logging::Level::Info,
            "Target TGI 00B1B104:61EFC000:719436BD observed at PFRecordRead boundary");
        LogTargetStack();
    }
    return result;
}

/** Observes the complete target property vector immediately before game publication. */
void __cdecl HookPublishParsedResource(
    void* resource,
    void* context,
    std::uint32_t notify) noexcept {
    if (resource != nullptr) {
        std::array<std::uint32_t, 3> identityWords{};
        std::memcpy(
            identityWords.data(), static_cast<const std::byte*>(resource) + 0x08U,
            sizeof(identityWords));
        const core::Tgi tgi{identityWords[1], identityWords[2], identityWords[0]};
        if (kTargetFilter.Matches(tgi)) {
            ParsedResourcePrefix prefix{};
            const bool prefixCopied = ReadCurrentProcess(
                reinterpret_cast<std::uintptr_t>(resource), &prefix, sizeof(prefix));
            std::array<core::RuntimePropertyRecord, kExpectedPropertyCount> properties{};
            const bool boundsValid = prefixCopied && prefix.reserved == 0U &&
                                     prefix.propertyBegin != 0U &&
                                     prefix.propertyEnd == prefix.propertyCapacity &&
                                     prefix.propertyEnd >= prefix.propertyBegin &&
                                     prefix.propertyEnd - prefix.propertyBegin ==
                                         properties.size() * sizeof(properties.front());
            const bool copied = boundsValid && ReadCurrentProcess(
                prefix.propertyBegin, properties.data(), sizeof(properties));
            const bool tableValid = copied &&
                core::ValidateRuntimePropertyTable(properties, kExpectedPropertyCount) ==
                    core::RuntimeTableStatus::Valid;
            std::array<float, kObservedProperties.size()> values{};
            bool valuesValid = tableValid;
            for (std::size_t index = 0; index < kObservedProperties.size() && valuesValid;
                 ++index) {
                const auto value =
                    core::ReadRuntimeFloat(properties, kObservedProperties[index]);
                valuesValid = value.has_value();
                if (value.has_value()) {
                    values[index] = value.value();
                }
            }
            bool baselineValid = valuesValid;
            for (std::size_t index = 0; index < values.size() && baselineValid; ++index) {
                baselineValid = values[index] == kExpectedPropertyValues[index];
            }
            if (valuesValid) {
                logging::AsyncLogger::Instance().WriteFormat(
                    logging::Level::Info,
                    "[SC13][RESOURCE] thread=%lu TGI=%s stage=parsed-before-publish "
                    "mode=observe-only resource=%p context=%p notify=%u records=%zu stride=%zu",
                    GetCurrentThreadId(), core::ToString(tgi).c_str(), resource, context, notify,
                    properties.size(), sizeof(properties.front()));
                for (std::size_t index = 0; index < kObservedProperties.size(); ++index) {
                    logging::AsyncLogger::Instance().WriteFormat(
                        logging::Level::Info, "[SC13][PROP] id=0x%08X float=%.3f",
                        kObservedProperties[index], values[index]);
                }
                logging::AsyncLogger::Instance().WriteFormat(
                    baselineValid ? logging::Level::Info : logging::Level::Error,
                    "[SC13][RESOURCE] runtime PROP baseline=%s expected=300/72/2/200; "
                    "mode=observe-only fail-open",
                    baselineValid ? "verified" : "mismatch");
                LogTargetStack();
            } else {
                logging::AsyncLogger::Instance().WriteFormat(
                    logging::Level::Error,
                    "[SC13][RESOURCE] target refused at parsed-before-publish: "
                    "resource=%p bounds=%s copied=%s table=%s values=%s; fail-open",
                    resource, boundsValid ? "valid" : "invalid", copied ? "yes" : "no",
                    tableValid ? "valid" : "invalid", valuesValid ? "valid" : "invalid");
            }
        }
    }
    g_originalPublishParsedResource(resource, context, notify);
}

}  // namespace

bool CreateResourceTraceHook(
    const reverse::BuildFingerprint& fingerprint, std::string& error) noexcept {
#if defined(SC13_ENABLE_DISCOVERY_TRACE)
    reverse::ResourceSymbols symbols{};
    if (!reverse::ResolveResourceSymbols(fingerprint, symbols, error)) {
        return false;
    }
    const MH_STATUS status = MH_CreateHook(
        symbols.pfRecordReadConstructor,
        reinterpret_cast<LPVOID>(&HookPFRecordReadConstructor),
        reinterpret_cast<LPVOID*>(&g_originalPFRecordReadConstructor));
    if (status != MH_OK) {
        error = std::string("MH_CreateHook failed for PFRecordRead: ") +
                MH_StatusToString(status);
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
#if defined(SC13_ENABLE_DISCOVERY_TRACE)
    logging::AsyncLogger::Instance().WriteFormat(
        logging::Level::Info,
        "Resolved PFRecordRead constructor uniquely: module=SimCity.exe RVA=0x%08X address=%p",
        symbols.pfRecordReadConstructorRva, symbols.pfRecordReadConstructor);
    logging::AsyncLogger::Instance().WriteFormat(
        logging::Level::Info,
        "Resolved parsed-resource publisher uniquely: module=SimCity.exe RVA=0x%08X address=%p",
        symbols.publishParsedResourceRva, symbols.publishParsedResource);
#endif
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
    RestoreRuntimePatches();
    g_originalPFRecordReadConstructor = nullptr;
    g_originalPublishParsedResource = nullptr;
    g_originalPropDeserialize = nullptr;
}

}  // namespace sc13::hooks
