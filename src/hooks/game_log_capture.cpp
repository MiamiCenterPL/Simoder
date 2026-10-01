#include "hooks/game_log_capture.hpp"

#include "logging/async_logger.hpp"

#include <Windows.h>
#include <MinHook.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstring>

namespace sc13::hooks {
namespace {

using OutputDebugStringAFunction = void(WINAPI*)(LPCSTR);
using OutputDebugStringWFunction = void(WINAPI*)(LPCWSTR);

constexpr std::size_t kMaximumDebugMessage = 4096U;
OutputDebugStringAFunction g_originalOutputDebugStringA = nullptr;
OutputDebugStringWFunction g_originalOutputDebugStringW = nullptr;
thread_local bool g_capturingDebugString = false;

/** Copies a bounded ANSI debug string through checked process-memory reads. */
[[nodiscard]] bool CopyAnsiMessage(
    LPCSTR source,
    std::array<char, kMaximumDebugMessage>& destination) noexcept {
    if (source == nullptr) {
        return false;
    }
    std::size_t used = 0U;
    while (used + 1U < destination.size()) {
        const std::uintptr_t address =
            reinterpret_cast<std::uintptr_t>(source) + used;
        MEMORY_BASIC_INFORMATION information{};
        if (VirtualQuery(
                reinterpret_cast<const void*>(address), &information,
                sizeof(information)) != sizeof(information) ||
            information.State != MEM_COMMIT ||
            (information.Protect & (PAGE_GUARD | PAGE_NOACCESS)) != 0U) {
            return false;
        }
        const std::uintptr_t regionEnd =
            reinterpret_cast<std::uintptr_t>(information.BaseAddress) +
            information.RegionSize;
        const std::size_t available = static_cast<std::size_t>(regionEnd - address);
        const std::size_t count =
            (std::min)(available, destination.size() - used - 1U);
        SIZE_T copied = 0U;
        if (count == 0U || ReadProcessMemory(
                GetCurrentProcess(), reinterpret_cast<const void*>(address),
                destination.data() + used, count, &copied) == FALSE ||
            copied != count) {
            return false;
        }
        const void* const terminator =
            std::memchr(destination.data() + used, '\0', count);
        if (terminator != nullptr) {
            return true;
        }
        used += count;
    }
    destination.back() = '\0';
    return true;
}

/** Copies and converts a bounded UTF-16 debug string to UTF-8. */
[[nodiscard]] bool CopyWideMessage(
    LPCWSTR source,
    std::array<char, kMaximumDebugMessage>& destination) noexcept {
    if (source == nullptr) {
        return false;
    }
    std::array<wchar_t, kMaximumDebugMessage / sizeof(wchar_t)> wide{};
    std::size_t used = 0U;
    while (used + 1U < wide.size()) {
        const std::uintptr_t address =
            reinterpret_cast<std::uintptr_t>(source + used);
        MEMORY_BASIC_INFORMATION information{};
        if (VirtualQuery(
                reinterpret_cast<const void*>(address), &information,
                sizeof(information)) != sizeof(information) ||
            information.State != MEM_COMMIT ||
            (information.Protect & (PAGE_GUARD | PAGE_NOACCESS)) != 0U) {
            return false;
        }
        const std::uintptr_t regionEnd =
            reinterpret_cast<std::uintptr_t>(information.BaseAddress) +
            information.RegionSize;
        const std::size_t availableCharacters =
            static_cast<std::size_t>(regionEnd - address) / sizeof(wchar_t);
        const std::size_t count =
            (std::min)(availableCharacters, wide.size() - used - 1U);
        SIZE_T copied = 0U;
        const std::size_t bytes = count * sizeof(wchar_t);
        if (count == 0U || ReadProcessMemory(
                GetCurrentProcess(), reinterpret_cast<const void*>(address),
                wide.data() + used, bytes, &copied) == FALSE || copied != bytes) {
            return false;
        }
        const wchar_t* const terminator = std::find(
            wide.data() + used, wide.data() + used + count, L'\0');
        if (terminator != wide.data() + used + count) {
            const int length = WideCharToMultiByte(
                CP_UTF8, WC_ERR_INVALID_CHARS, wide.data(),
                static_cast<int>(terminator - wide.data()), destination.data(),
                static_cast<int>(destination.size() - 1U), nullptr, nullptr);
            if (length <= 0) {
                return false;
            }
            destination[static_cast<std::size_t>(length)] = '\0';
            return true;
        }
        used += count;
    }
    wide.back() = L'\0';
    const int length = WideCharToMultiByte(
        CP_UTF8, 0, wide.data(), -1, destination.data(),
        static_cast<int>(destination.size()), nullptr, nullptr);
    return length > 0;
}

/** Preserves ANSI debugger output while forwarding a bounded copy to the logger. */
void WINAPI HookOutputDebugStringA(LPCSTR message) noexcept {
    g_originalOutputDebugStringA(message);
    const DWORD apiError = GetLastError();
    if (!g_capturingDebugString) {
        g_capturingDebugString = true;
        std::array<char, kMaximumDebugMessage> copy{};
        if (CopyAnsiMessage(message, copy)) {
            logging::AsyncLogger::Instance().WriteEvent(
                logging::Level::Info, logging::SourceType::Game,
                "OutputDebugStringA", copy.data());
        }
        g_capturingDebugString = false;
    }
    SetLastError(apiError);
}

/** Preserves UTF-16 debugger output while forwarding a UTF-8 copy to the logger. */
void WINAPI HookOutputDebugStringW(LPCWSTR message) noexcept {
    g_originalOutputDebugStringW(message);
    const DWORD apiError = GetLastError();
    if (!g_capturingDebugString) {
        g_capturingDebugString = true;
        std::array<char, kMaximumDebugMessage> copy{};
        if (CopyWideMessage(message, copy)) {
            logging::AsyncLogger::Instance().WriteEvent(
                logging::Level::Info, logging::SourceType::Game,
                "OutputDebugStringW", copy.data());
        }
        g_capturingDebugString = false;
    }
    SetLastError(apiError);
}

/** Creates one kernel32 API hook and returns a stable diagnostic. */
[[nodiscard]] bool CreateApiHook(
    const char* name,
    LPVOID detour,
    LPVOID* original,
    std::string& error) {
    const MH_STATUS status = MH_CreateHookApi(L"kernel32.dll", name, detour, original);
    if (status != MH_OK) {
        error = std::string("MH_CreateHookApi failed for ") + name + ": " +
                MH_StatusToString(status);
        return false;
    }
    return true;
}

}  // namespace

bool CreateGameLogCaptureHooks(std::string& error) noexcept {
    try {
        return CreateApiHook(
                   "OutputDebugStringA",
                   reinterpret_cast<LPVOID>(&HookOutputDebugStringA),
                   reinterpret_cast<LPVOID*>(&g_originalOutputDebugStringA), error) &&
               CreateApiHook(
                   "OutputDebugStringW",
                   reinterpret_cast<LPVOID>(&HookOutputDebugStringW),
                   reinterpret_cast<LPVOID*>(&g_originalOutputDebugStringW), error);
    } catch (...) {
        error = "OutputDebugString hook setup failed due to an allocation exception";
        return false;
    }
}

void ResetGameLogCapture() noexcept {
    g_originalOutputDebugStringA = nullptr;
    g_originalOutputDebugStringW = nullptr;
    g_capturingDebugString = false;
}

}  // namespace sc13::hooks
