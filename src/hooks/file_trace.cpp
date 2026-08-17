#include "hooks/file_trace.hpp"

#include "hooks/resource_trace.hpp"
#include "logging/async_logger.hpp"
#include "reverse/resource_symbols.hpp"

#include <Windows.h>
#include <MinHook.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <cwchar>
#include <intrin.h>

namespace sc13::hooks {
namespace {

using CreateFileWFunction = HANDLE(WINAPI*)(
    LPCWSTR, DWORD, DWORD, LPSECURITY_ATTRIBUTES, DWORD, DWORD, HANDLE);
using CreateFileAFunction = HANDLE(WINAPI*)(
    LPCSTR, DWORD, DWORD, LPSECURITY_ATTRIBUTES, DWORD, DWORD, HANDLE);
using ReadFileFunction = BOOL(WINAPI*)(HANDLE, LPVOID, DWORD, LPDWORD, LPOVERLAPPED);
using SetFilePointerFunction = DWORD(WINAPI*)(HANDLE, LONG, PLONG, DWORD);
using SetFilePointerExFunction = BOOL(WINAPI*)(HANDLE, LARGE_INTEGER, PLARGE_INTEGER, DWORD);
using CreateFileMappingWFunction = HANDLE(WINAPI*)(
    HANDLE, LPSECURITY_ATTRIBUTES, DWORD, DWORD, DWORD, LPCWSTR);
using CreateFileMappingAFunction = HANDLE(WINAPI*)(
    HANDLE, LPSECURITY_ATTRIBUTES, DWORD, DWORD, DWORD, LPCSTR);
using MapViewOfFileFunction = LPVOID(WINAPI*)(HANDLE, DWORD, DWORD, DWORD, SIZE_T);
using CloseHandleFunction = BOOL(WINAPI*)(HANDLE);
using StreamReadFunction = std::int32_t(__thiscall*)(void*, void*, std::uint32_t);

constexpr std::size_t kTrackedFileCapacity = 256;
constexpr std::size_t kTrackedMappingCapacity = 256;
constexpr std::size_t kPathCapacity = 1024;
constexpr std::uint32_t kMaximumReadEventsPerFile = 16;
constexpr std::uint32_t kMaximumSeekEventsPerFile = 16;
constexpr std::uint64_t kTargetResourceOffset = 0x00059A7EULL;
constexpr std::uint64_t kTargetResourceDiskSize = 879ULL;
constexpr wchar_t kTargetPackageName[] = L"SimCityDLCEP1-Scripts_287520926.package";

/** Stores one package handle without allocating in a hook. */
struct TrackedFile {
    HANDLE handle{};
    std::array<wchar_t, kPathCapacity> path{};
    std::uint32_t readEvents{};
    std::uint32_t seekEvents{};
};

/** Associates a file mapping with the package file that created it. */
struct TrackedMapping {
    HANDLE mapping{};
    HANDLE file{};
};

SRWLOCK g_stateLock = SRWLOCK_INIT;
std::array<TrackedFile, kTrackedFileCapacity> g_files{};
std::array<TrackedMapping, kTrackedMappingCapacity> g_mappings{};
thread_local bool g_insideTrace = false;
bool g_installed = false;

CreateFileWFunction g_originalCreateFileW = nullptr;
CreateFileAFunction g_originalCreateFileA = nullptr;
ReadFileFunction g_originalReadFile = nullptr;
SetFilePointerFunction g_originalSetFilePointer = nullptr;
SetFilePointerExFunction g_originalSetFilePointerEx = nullptr;
CreateFileMappingWFunction g_originalCreateFileMappingW = nullptr;
CreateFileMappingAFunction g_originalCreateFileMappingA = nullptr;
MapViewOfFileFunction g_originalMapViewOfFile = nullptr;
CloseHandleFunction g_originalCloseHandle = nullptr;
StreamReadFunction g_originalStreamRead = nullptr;

/** Prevents logging helpers from recursively re-entering traced APIs. */
class ReentryGuard final {
public:
    ReentryGuard() noexcept : entered_(!g_insideTrace) {
        if (entered_) {
            g_insideTrace = true;
        }
    }

    /** Restores the thread-local tracing state. */
    ~ReentryGuard() {
        if (entered_) {
            g_insideTrace = false;
        }
    }

    /** Returns true only for the outermost traced call. */
    [[nodiscard]] bool entered() const noexcept { return entered_; }

private:
    bool entered_{};
};

/** Returns true only for the canonical package known to contain the target resource. */
[[nodiscard]] bool IsInterestingPackagePath(const wchar_t* path) noexcept {
    if (path == nullptr) {
        return false;
    }
    const wchar_t* fileName = path;
    if (const wchar_t* const slash = std::wcsrchr(path, L'\\'); slash != nullptr) {
        fileName = slash + 1;
    }
    if (const wchar_t* const slash = std::wcsrchr(fileName, L'/'); slash != nullptr) {
        fileName = slash + 1;
    }
    return _wcsicmp(fileName, kTargetPackageName) == 0;
}

/** Converts an ANSI file path using the process ANSI code page. */
[[nodiscard]] bool ConvertPath(const char* source, std::array<wchar_t, kPathCapacity>& target) noexcept {
    if (source == nullptr) {
        return false;
    }
    const int converted = MultiByteToWideChar(
        CP_ACP, MB_ERR_INVALID_CHARS, source, -1, target.data(), static_cast<int>(target.size()));
    return converted > 0;
}

/** Adds or refreshes one package file handle in the bounded table. */
void TrackFile(HANDLE handle, const wchar_t* path) noexcept {
    if (handle == nullptr || handle == INVALID_HANDLE_VALUE || path == nullptr) {
        return;
    }
    AcquireSRWLockExclusive(&g_stateLock);
    TrackedFile* destination = nullptr;
    for (TrackedFile& file : g_files) {
        if (file.handle == handle) {
            destination = &file;
            break;
        }
        if (destination == nullptr && file.handle == nullptr) {
            destination = &file;
        }
    }
    if (destination != nullptr) {
        destination->handle = handle;
        destination->readEvents = 0;
        destination->seekEvents = 0;
        wcsncpy_s(destination->path.data(), destination->path.size(), path, _TRUNCATE);
    }
    ReleaseSRWLockExclusive(&g_stateLock);
}

/** Reserves a bounded seek event while always allowing the exact target offset. */
[[nodiscard]] bool ReserveSeekEvent(HANDLE handle, std::uint64_t position) noexcept {
    bool shouldLog = position == kTargetResourceOffset;
    AcquireSRWLockExclusive(&g_stateLock);
    for (TrackedFile& file : g_files) {
        if (file.handle == handle) {
            if (file.seekEvents < kMaximumSeekEventsPerFile) {
                ++file.seekEvents;
                shouldLog = true;
            }
            break;
        }
    }
    ReleaseSRWLockExclusive(&g_stateLock);
    return shouldLog;
}

/** Copies a tracked package path and optionally reserves one bounded read event. */
[[nodiscard]] bool FindFile(
    HANDLE handle,
    std::array<wchar_t, kPathCapacity>& path,
    bool reserveReadEvent,
    bool* eventReserved = nullptr) noexcept {
    bool found = false;
    if (eventReserved != nullptr) {
        *eventReserved = false;
    }
    AcquireSRWLockExclusive(&g_stateLock);
    for (TrackedFile& file : g_files) {
        if (file.handle == handle) {
            wcsncpy_s(path.data(), path.size(), file.path.data(), _TRUNCATE);
            found = true;
            if (reserveReadEvent && file.readEvents < kMaximumReadEventsPerFile) {
                ++file.readEvents;
                if (eventReserved != nullptr) {
                    *eventReserved = true;
                }
            }
            break;
        }
    }
    ReleaseSRWLockExclusive(&g_stateLock);
    return found;
}

/** Associates a mapping handle with a tracked package file. */
void TrackMapping(HANDLE mapping, HANDLE file) noexcept {
    if (mapping == nullptr || mapping == INVALID_HANDLE_VALUE) {
        return;
    }
    AcquireSRWLockExclusive(&g_stateLock);
    TrackedMapping* destination = nullptr;
    for (TrackedMapping& item : g_mappings) {
        if (item.mapping == mapping) {
            destination = &item;
            break;
        }
        if (destination == nullptr && item.mapping == nullptr) {
            destination = &item;
        }
    }
    if (destination != nullptr) {
        destination->mapping = mapping;
        destination->file = file;
    }
    ReleaseSRWLockExclusive(&g_stateLock);
}

/** Resolves a mapping handle back to its package path. */
[[nodiscard]] bool FindMapping(
    HANDLE mapping, std::array<wchar_t, kPathCapacity>& path) noexcept {
    HANDLE file = nullptr;
    AcquireSRWLockShared(&g_stateLock);
    for (const TrackedMapping& item : g_mappings) {
        if (item.mapping == mapping) {
            file = item.file;
            break;
        }
    }
    ReleaseSRWLockShared(&g_stateLock);
    return file != nullptr && FindFile(file, path, false);
}

/** Removes a closed file or mapping handle from both bounded tables. */
void ForgetHandle(HANDLE handle) noexcept {
    AcquireSRWLockExclusive(&g_stateLock);
    for (TrackedFile& file : g_files) {
        if (file.handle == handle) {
            file = {};
        }
    }
    for (TrackedMapping& mapping : g_mappings) {
        if (mapping.mapping == handle || mapping.file == handle) {
            mapping = {};
        }
    }
    ReleaseSRWLockExclusive(&g_stateLock);
}

/** Logs a short module-relative stack for a filtered package event. */
void LogCallStack() noexcept {
    std::array<void*, 12> frames{};
    const USHORT captured = CaptureStackBackTrace(2, static_cast<DWORD>(frames.size()), frames.data(), nullptr);
    for (USHORT index = 0; index < captured; ++index) {
        HMODULE module = nullptr;
        if (GetModuleHandleExW(
                GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                reinterpret_cast<LPCWSTR>(frames[index]), &module) == FALSE) {
            logging::AsyncLogger::Instance().WriteFormat(
                logging::Level::Trace, "  stack[%u] address=%p module=unknown", index, frames[index]);
            continue;
        }
        std::array<wchar_t, kPathCapacity> modulePath{};
        GetModuleFileNameW(module, modulePath.data(), static_cast<DWORD>(modulePath.size()));
        const auto rva = reinterpret_cast<std::uintptr_t>(frames[index]) -
                         reinterpret_cast<std::uintptr_t>(module);
        logging::AsyncLogger::Instance().WriteFormat(
            logging::Level::Trace, "  stack[%u] %ls+0x%08llX", index, modulePath.data(),
            static_cast<unsigned long long>(rva));
    }
}

/** Records a successful package open and its caller stack. */
void RecordOpen(HANDLE handle, const wchar_t* path) noexcept {
    if (!IsInterestingPackagePath(path)) {
        return;
    }
    TrackFile(handle, path);
    logging::AsyncLogger::Instance().WriteFormat(
        logging::Level::Info, "Target package opened: thread=%lu handle=%p path=%ls",
        GetCurrentThreadId(), handle, path);
    LogCallStack();
}

/** Queries the current synchronous file position through the unhooked trampoline. */
[[nodiscard]] bool QueryCurrentOffset(HANDLE file, std::uint64_t& offset) noexcept {
    if (g_originalSetFilePointerEx == nullptr) {
        return false;
    }
    LARGE_INTEGER zero{};
    LARGE_INTEGER current{};
    if (g_originalSetFilePointerEx(file, zero, &current, FILE_CURRENT) == FALSE ||
        current.QuadPart < 0) {
        return false;
    }
    offset = static_cast<std::uint64_t>(current.QuadPart);
    return true;
}

/** Reports whether one successful read overlaps the exact compressed target payload. */
[[nodiscard]] bool OverlapsTargetResource(
    std::uint64_t offset,
    std::uint64_t size) noexcept {
    constexpr std::uint64_t targetEnd = kTargetResourceOffset + kTargetResourceDiskSize;
    return size != 0U && offset < targetEnd && size <= UINT64_MAX - offset &&
           offset + size > kTargetResourceOffset;
}

/** Intercepts wide-character file opens and filters them to package paths. */
HANDLE WINAPI HookCreateFileW(
    LPCWSTR fileName,
    DWORD desiredAccess,
    DWORD shareMode,
    LPSECURITY_ATTRIBUTES security,
    DWORD creationDisposition,
    DWORD flags,
    HANDLE templateFile) noexcept {
    const HANDLE result = g_originalCreateFileW(
        fileName, desiredAccess, shareMode, security, creationDisposition, flags, templateFile);
    const DWORD apiError = GetLastError();
    ReentryGuard guard;
    if (guard.entered() && result != INVALID_HANDLE_VALUE) {
        RecordOpen(result, fileName);
    }
    SetLastError(apiError);
    return result;
}

/** Intercepts ANSI file opens and normalizes filtered package paths to UTF-16. */
HANDLE WINAPI HookCreateFileA(
    LPCSTR fileName,
    DWORD desiredAccess,
    DWORD shareMode,
    LPSECURITY_ATTRIBUTES security,
    DWORD creationDisposition,
    DWORD flags,
    HANDLE templateFile) noexcept {
    const HANDLE result = g_originalCreateFileA(
        fileName, desiredAccess, shareMode, security, creationDisposition, flags, templateFile);
    const DWORD apiError = GetLastError();
    ReentryGuard guard;
    if (guard.entered() && result != INVALID_HANDLE_VALUE) {
        std::array<wchar_t, kPathCapacity> path{};
        if (ConvertPath(fileName, path)) {
            RecordOpen(result, path.data());
        }
    }
    SetLastError(apiError);
    return result;
}

/** Intercepts reads from tracked package handles with a strict per-file event limit. */
BOOL WINAPI HookReadFile(
    HANDLE file,
    LPVOID buffer,
    DWORD bytesToRead,
    LPDWORD bytesRead,
    LPOVERLAPPED overlapped) noexcept {
    std::array<wchar_t, kPathCapacity> trackedPath{};
    const bool tracked = FindFile(file, trackedPath, false);
    std::uint64_t synchronousOffset = 0;
    const bool synchronousOffsetKnown =
        tracked && overlapped == nullptr && QueryCurrentOffset(file, synchronousOffset);
    const BOOL result = g_originalReadFile(file, buffer, bytesToRead, bytesRead, overlapped);
    const DWORD apiError = GetLastError();
    ReentryGuard guard;
    if (guard.entered() && tracked) {
        std::array<wchar_t, kPathCapacity> path{};
        bool eventReserved = false;
        if (FindFile(file, path, true, &eventReserved)) {
            const DWORD actual = bytesRead != nullptr ? *bytesRead : 0;
            const std::uint64_t offset = overlapped != nullptr ?
                (static_cast<std::uint64_t>(overlapped->OffsetHigh) << 32U) | overlapped->Offset :
                synchronousOffset;
            const bool offsetKnown = overlapped != nullptr || synchronousOffsetKnown;
            const bool targetRange =
                result != FALSE && offsetKnown && OverlapsTargetResource(offset, actual);
            if (eventReserved || targetRange) {
                logging::AsyncLogger::Instance().WriteFormat(
                    targetRange ? logging::Level::Info : logging::Level::Trace,
                    "Target package read: thread=%lu handle=%p offset=%s0x%llX "
                    "requested=%lu actual=%lu targetRange=%s path=%ls",
                    GetCurrentThreadId(), file, offsetKnown ? "" : "unknown/",
                    static_cast<unsigned long long>(offset), bytesToRead, actual,
                    targetRange ? "yes" : "no", path.data());
                if (targetRange) {
                    LogCallStack();
                }
            }
        }
    }
    SetLastError(apiError);
    return result;
}

/** Intercepts legacy file seeks for the one tracked package. */
DWORD WINAPI HookSetFilePointer(
    HANDLE file,
    LONG distanceLow,
    PLONG distanceHigh,
    DWORD moveMethod) noexcept {
    const DWORD result =
        g_originalSetFilePointer(file, distanceLow, distanceHigh, moveMethod);
    const DWORD apiError = GetLastError();
    ReentryGuard guard;
    if (guard.entered()) {
        std::array<wchar_t, kPathCapacity> path{};
        if (FindFile(file, path, false) &&
            !(result == INVALID_SET_FILE_POINTER && apiError != NO_ERROR)) {
            const std::uint64_t position = distanceHigh != nullptr ?
                (static_cast<std::uint64_t>(static_cast<std::uint32_t>(*distanceHigh)) << 32U) |
                    result : result;
            if (ReserveSeekEvent(file, position)) {
                logging::AsyncLogger::Instance().WriteFormat(
                    position == kTargetResourceOffset ? logging::Level::Info :
                                                        logging::Level::Trace,
                    "Target package seek: thread=%lu api=SetFilePointer handle=%p "
                    "position=0x%llX method=%lu path=%ls",
                    GetCurrentThreadId(), file, static_cast<unsigned long long>(position),
                    moveMethod, path.data());
            }
        }
    }
    SetLastError(apiError);
    return result;
}

/** Intercepts 64-bit file seeks for the one tracked package. */
BOOL WINAPI HookSetFilePointerEx(
    HANDLE file,
    LARGE_INTEGER distance,
    PLARGE_INTEGER newPosition,
    DWORD moveMethod) noexcept {
    const BOOL result = g_originalSetFilePointerEx(file, distance, newPosition, moveMethod);
    const DWORD apiError = GetLastError();
    ReentryGuard guard;
    if (guard.entered() && result != FALSE) {
        std::array<wchar_t, kPathCapacity> path{};
        if (FindFile(file, path, false)) {
            const long long position = newPosition != nullptr ? newPosition->QuadPart : -1LL;
            if (position >= 0 && ReserveSeekEvent(file, static_cast<std::uint64_t>(position))) {
                logging::AsyncLogger::Instance().WriteFormat(
                    static_cast<std::uint64_t>(position) == kTargetResourceOffset ?
                        logging::Level::Info : logging::Level::Trace,
                    "Target package seek: thread=%lu api=SetFilePointerEx handle=%p "
                    "position=0x%llX method=%lu path=%ls",
                    GetCurrentThreadId(), file, static_cast<unsigned long long>(position),
                    moveMethod, path.data());
            }
        }
    }
    SetLastError(apiError);
    return result;
}

/** Intercepts wide-character file-mapping creation for tracked packages. */
HANDLE WINAPI HookCreateFileMappingW(
    HANDLE file,
    LPSECURITY_ATTRIBUTES security,
    DWORD protection,
    DWORD maximumSizeHigh,
    DWORD maximumSizeLow,
    LPCWSTR name) noexcept {
    const HANDLE mapping = g_originalCreateFileMappingW(
        file, security, protection, maximumSizeHigh, maximumSizeLow, name);
    ReentryGuard guard;
    if (guard.entered() && mapping != nullptr) {
        std::array<wchar_t, kPathCapacity> path{};
        if (FindFile(file, path, false)) {
            TrackMapping(mapping, file);
            logging::AsyncLogger::Instance().WriteFormat(
                logging::Level::Trace, "Package mapping created: mapping=%p file=%p path=%ls",
                mapping, file, path.data());
        }
    }
    return mapping;
}

/** Intercepts ANSI file-mapping creation for tracked packages. */
HANDLE WINAPI HookCreateFileMappingA(
    HANDLE file,
    LPSECURITY_ATTRIBUTES security,
    DWORD protection,
    DWORD maximumSizeHigh,
    DWORD maximumSizeLow,
    LPCSTR name) noexcept {
    const HANDLE mapping = g_originalCreateFileMappingA(
        file, security, protection, maximumSizeHigh, maximumSizeLow, name);
    ReentryGuard guard;
    if (guard.entered() && mapping != nullptr) {
        std::array<wchar_t, kPathCapacity> path{};
        if (FindFile(file, path, false)) {
            TrackMapping(mapping, file);
            logging::AsyncLogger::Instance().WriteFormat(
                logging::Level::Trace, "Package mapping created: mapping=%p file=%p path=%ls",
                mapping, file, path.data());
        }
    }
    return mapping;
}

/** Intercepts mapped views backed by tracked package files. */
LPVOID WINAPI HookMapViewOfFile(
    HANDLE mapping,
    DWORD desiredAccess,
    DWORD fileOffsetHigh,
    DWORD fileOffsetLow,
    SIZE_T bytesToMap) noexcept {
    LPVOID view = g_originalMapViewOfFile(
        mapping, desiredAccess, fileOffsetHigh, fileOffsetLow, bytesToMap);
    ReentryGuard guard;
    if (guard.entered() && view != nullptr) {
        std::array<wchar_t, kPathCapacity> path{};
        if (FindMapping(mapping, path)) {
            const unsigned long long offset =
                (static_cast<unsigned long long>(fileOffsetHigh) << 32U) | fileOffsetLow;
            logging::AsyncLogger::Instance().WriteFormat(
                logging::Level::Trace,
                "Package view mapped: mapping=%p view=%p offset=0x%llX size=%llu path=%ls",
                mapping, view, offset, static_cast<unsigned long long>(bytesToMap), path.data());
        }
    }
    return view;
}

/** Removes closed handles from the tracing tables after preserving API behavior. */
BOOL WINAPI HookCloseHandle(HANDLE handle) noexcept {
    const BOOL result = g_originalCloseHandle(handle);
    ReentryGuard guard;
    if (guard.entered() && result != FALSE) {
        ForgetHandle(handle);
    }
    return result;
}

/** Intercepts the verified game stream wrapper and logs only the target DBPF byte range. */
std::int32_t __fastcall HookStreamRead(
    void* self,
    void*,
    void* buffer,
    std::uint32_t requested) noexcept {
    HANDLE file = nullptr;
    if (self != nullptr) {
        std::memcpy(&file, static_cast<const std::byte*>(self) + 0x04U, sizeof(file));
    }
    std::array<wchar_t, kPathCapacity> path{};
    const bool tracked = file != nullptr && FindFile(file, path, false);
    std::uint64_t offset = 0;
    const bool offsetKnown = tracked && QueryCurrentOffset(file, offset);
    void* const caller = _ReturnAddress();
    const std::int32_t result = g_originalStreamRead(self, buffer, requested);
    if (tracked && offsetKnown && result > 0 &&
        OverlapsTargetResource(offset, static_cast<std::uint64_t>(result))) {
        HMODULE module = GetModuleHandleW(nullptr);
        const std::uintptr_t callerRva = reinterpret_cast<std::uintptr_t>(caller) -
                                         reinterpret_cast<std::uintptr_t>(module);
        logging::AsyncLogger::Instance().WriteFormat(
            logging::Level::Info,
            "[SC13][STREAM] thread=%lu offset=0x%llX requested=%u actual=%d "
            "caller=SimCity.exe+0x%08llX stream=%p buffer=%p path=%ls",
            GetCurrentThreadId(), static_cast<unsigned long long>(offset), requested, result,
            static_cast<unsigned long long>(callerRva), self, buffer, path.data());
    }
    return result;
}

/** Creates the exact-build stream wrapper hook without enabling MinHook globally. */
[[nodiscard]] bool CreateStreamReadHook(
    const reverse::BuildFingerprint& fingerprint,
    std::string& error) {
    reverse::ResourceSymbols symbols{};
    if (!reverse::ResolveResourceSymbols(fingerprint, symbols, error)) {
        return false;
    }
    const MH_STATUS status = MH_CreateHook(
        symbols.streamRead, reinterpret_cast<LPVOID>(&HookStreamRead),
        reinterpret_cast<LPVOID*>(&g_originalStreamRead));
    if (status != MH_OK) {
        error = std::string("MH_CreateHook failed for stream read wrapper: ") +
                MH_StatusToString(status);
        return false;
    }
    logging::AsyncLogger::Instance().WriteFormat(
        logging::Level::Info,
        "Resolved stream read wrapper uniquely: module=SimCity.exe RVA=0x%08X address=%p",
        symbols.streamReadRva, symbols.streamRead);
    return true;
}

/** Creates one MinHook API hook and produces a readable error on failure. */
[[nodiscard]] bool CreateApiHook(
    const char* name, LPVOID detour, LPVOID* original, std::string& error) {
    const MH_STATUS status = MH_CreateHookApi(L"kernel32.dll", name, detour, original);
    if (status != MH_OK) {
        error = std::string("MH_CreateHookApi failed for ") + name + ": " + MH_StatusToString(status);
        return false;
    }
    return true;
}

}  // namespace

bool InstallTraceHooks(
    const reverse::BuildFingerprint& fingerprint, std::string& error) noexcept {
    try {
        if (g_installed) {
            return true;
        }
        const MH_STATUS initializeStatus = MH_Initialize();
        if (initializeStatus != MH_OK) {
            error = std::string("MH_Initialize failed: ") + MH_StatusToString(initializeStatus);
            return false;
        }

        const bool created =
#if defined(SC13_ENABLE_DISCOVERY_TRACE)
            CreateApiHook("CreateFileW", reinterpret_cast<LPVOID>(&HookCreateFileW),
                          reinterpret_cast<LPVOID*>(&g_originalCreateFileW), error) &&
            CreateApiHook("CreateFileA", reinterpret_cast<LPVOID>(&HookCreateFileA),
                          reinterpret_cast<LPVOID*>(&g_originalCreateFileA), error) &&
            CreateApiHook("ReadFile", reinterpret_cast<LPVOID>(&HookReadFile),
                          reinterpret_cast<LPVOID*>(&g_originalReadFile), error) &&
            CreateApiHook("SetFilePointer", reinterpret_cast<LPVOID>(&HookSetFilePointer),
                          reinterpret_cast<LPVOID*>(&g_originalSetFilePointer), error) &&
            CreateApiHook("SetFilePointerEx", reinterpret_cast<LPVOID>(&HookSetFilePointerEx),
                          reinterpret_cast<LPVOID*>(&g_originalSetFilePointerEx), error) &&
            CreateApiHook("CreateFileMappingW", reinterpret_cast<LPVOID>(&HookCreateFileMappingW),
                          reinterpret_cast<LPVOID*>(&g_originalCreateFileMappingW), error) &&
            CreateApiHook("CreateFileMappingA", reinterpret_cast<LPVOID>(&HookCreateFileMappingA),
                          reinterpret_cast<LPVOID*>(&g_originalCreateFileMappingA), error) &&
            CreateApiHook("MapViewOfFile", reinterpret_cast<LPVOID>(&HookMapViewOfFile),
                          reinterpret_cast<LPVOID*>(&g_originalMapViewOfFile), error) &&
            CreateApiHook("CloseHandle", reinterpret_cast<LPVOID>(&HookCloseHandle),
                          reinterpret_cast<LPVOID*>(&g_originalCloseHandle), error) &&
            CreateStreamReadHook(fingerprint, error) &&
            CreateResourceTraceHook(fingerprint, error);
#else
            CreateResourceTraceHook(fingerprint, error);
#endif
        if (!created) {
            MH_RemoveHook(MH_ALL_HOOKS);
            MH_Uninitialize();
            ResetResourceTrace();
            return false;
        }

        const MH_STATUS enableStatus = MH_EnableHook(MH_ALL_HOOKS);
        if (enableStatus != MH_OK) {
            error = std::string("MH_EnableHook failed: ") + MH_StatusToString(enableStatus);
            MH_RemoveHook(MH_ALL_HOOKS);
            MH_Uninitialize();
            ResetResourceTrace();
            return false;
        }
        g_installed = true;
        return true;
    } catch (...) {
        error = "Runtime resource hook setup failed due to an allocation exception";
        MH_RemoveHook(MH_ALL_HOOKS);
        MH_Uninitialize();
        ResetResourceTrace();
        return false;
    }
}

void UninstallTraceHooks() noexcept {
    if (!g_installed) {
        return;
    }
    MH_DisableHook(MH_ALL_HOOKS);
    MH_RemoveHook(MH_ALL_HOOKS);
    MH_Uninitialize();
    ResetResourceTrace();
    g_originalStreamRead = nullptr;
    g_installed = false;
}

}  // namespace sc13::hooks
