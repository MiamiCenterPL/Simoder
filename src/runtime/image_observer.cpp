#include "runtime/image_observer.hpp"

#include "core/sha256.hpp"
#include "logging/async_logger.hpp"

#include <Windows.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <exception>
#include <filesystem>
#include <fstream>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace sc13::runtime {
namespace {

/** Describes the mapped executable code section watched by the observer. */
struct TextRegion final {
    const std::byte* address = nullptr;
    std::uint32_t rva = 0;
    std::uint32_t size = 0;
};

/** Stores process-wide observer state controlled by the exported lifecycle functions. */
struct ObserverState final {
    std::atomic_bool stopRequested = false;
    std::atomic_bool running = false;
    std::thread worker;
};

ObserverState g_observer;

/** Returns true when a page protection permits ordinary reads. */
[[nodiscard]] bool IsReadableProtection(const DWORD protection) noexcept {
    if ((protection & (PAGE_GUARD | PAGE_NOACCESS)) != 0) {
        return false;
    }
    const DWORD baseProtection = protection & 0xFFU;
    return baseProtection == PAGE_READONLY || baseProtection == PAGE_READWRITE ||
           baseProtection == PAGE_WRITECOPY || baseProtection == PAGE_EXECUTE_READ ||
           baseProtection == PAGE_EXECUTE_READWRITE || baseProtection == PAGE_EXECUTE_WRITECOPY;
}

/** Validates that every page covering a memory range is committed and readable. */
[[nodiscard]] bool IsReadableRange(const std::byte* address, const std::size_t size) noexcept {
    if (address == nullptr || size == 0) {
        return false;
    }

    const std::byte* cursor = address;
    const std::byte* const end = address + size;
    while (cursor < end) {
        MEMORY_BASIC_INFORMATION information{};
        if (VirtualQuery(cursor, &information, sizeof(information)) != sizeof(information) ||
            information.State != MEM_COMMIT || !IsReadableProtection(information.Protect)) {
            return false;
        }
        const auto regionEnd = static_cast<const std::byte*>(information.BaseAddress) +
                               information.RegionSize;
        if (regionEnd <= cursor) {
            return false;
        }
        cursor = std::min(regionEnd, end);
    }
    return true;
}

/** Locates and validates the loaded PE32 executable .text section. */
[[nodiscard]] bool FindTextRegion(
    HMODULE module, TextRegion& region, std::string& error) noexcept {
    if (module == nullptr) {
        error = "Executable module handle is null";
        return false;
    }

    const auto* const base = reinterpret_cast<const std::byte*>(module);
    if (!IsReadableRange(base, sizeof(IMAGE_DOS_HEADER))) {
        error = "Executable DOS header is unreadable";
        return false;
    }
    const auto* const dosHeader = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    if (dosHeader->e_magic != IMAGE_DOS_SIGNATURE || dosHeader->e_lfanew <= 0) {
        error = "Executable DOS header is invalid";
        return false;
    }

    const auto* const ntHeaders = reinterpret_cast<const IMAGE_NT_HEADERS32*>(
        base + static_cast<std::size_t>(dosHeader->e_lfanew));
    if (!IsReadableRange(reinterpret_cast<const std::byte*>(ntHeaders), sizeof(*ntHeaders)) ||
        ntHeaders->Signature != IMAGE_NT_SIGNATURE ||
        ntHeaders->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR32_MAGIC) {
        error = "Executable PE32 headers are invalid";
        return false;
    }

    const IMAGE_SECTION_HEADER* const sections = IMAGE_FIRST_SECTION(ntHeaders);
    const std::size_t sectionBytes =
        static_cast<std::size_t>(ntHeaders->FileHeader.NumberOfSections) * sizeof(*sections);
    if (!IsReadableRange(reinterpret_cast<const std::byte*>(sections), sectionBytes)) {
        error = "Executable section table is unreadable";
        return false;
    }

    for (WORD index = 0; index < ntHeaders->FileHeader.NumberOfSections; ++index) {
        std::array<char, IMAGE_SIZEOF_SHORT_NAME + 1> name{};
        std::memcpy(name.data(), sections[index].Name, IMAGE_SIZEOF_SHORT_NAME);
        if (std::string_view(name.data()) != ".text") {
            continue;
        }

        const std::uint32_t size = sections[index].Misc.VirtualSize;
        const std::uint32_t rva = sections[index].VirtualAddress;
        if (size == 0 || rva > ntHeaders->OptionalHeader.SizeOfImage ||
            size > ntHeaders->OptionalHeader.SizeOfImage - rva ||
            !IsReadableRange(base + rva, size)) {
            error = "Loaded .text section is outside the readable image";
            return false;
        }
        region = TextRegion{base + rva, rva, size};
        return true;
    }

    error = "Executable has no .text section";
    return false;
}

/** Computes a low-cost sample fingerprint spread across the complete .text section. */
[[nodiscard]] std::uint64_t SampleFingerprint(const TextRegion& region) noexcept {
    constexpr std::size_t sampleCount = 32;
    constexpr std::size_t sampleBytes = 256;
    constexpr std::uint64_t offsetBasis = 14695981039346656037ULL;
    constexpr std::uint64_t prime = 1099511628211ULL;

    std::uint64_t hash = offsetBasis;
    const std::size_t usableBytes = std::min<std::size_t>(sampleBytes, region.size);
    for (std::size_t sample = 0; sample < sampleCount; ++sample) {
        const std::size_t maximumOffset = region.size - usableBytes;
        const std::size_t offset = maximumOffset * sample / (sampleCount - 1);
        for (std::size_t index = 0; index < usableBytes; ++index) {
            hash ^= std::to_integer<std::uint8_t>(region.address[offset + index]);
            hash *= prime;
        }
    }
    return hash;
}

/** Copies the current .text bytes and writes a digest-addressed diagnostic capture. */
[[nodiscard]] bool CaptureText(
    const TextRegion& region,
    const std::filesystem::path& directory,
    const std::wstring_view reason,
    std::string& digestHex,
    std::filesystem::path& outputPath,
    std::string& error) noexcept {
    try {
        std::vector<std::byte> bytes(region.size);
        std::memcpy(bytes.data(), region.address, bytes.size());

        core::Sha256Digest digest{};
        if (!core::Sha256(bytes, digest, error)) {
            return false;
        }
        digestHex = core::ToHex(digest);

        std::error_code directoryError;
        std::filesystem::create_directories(directory, directoryError);
        if (directoryError) {
            error = "Could not create runtime capture directory: " + directoryError.message();
            return false;
        }

        const std::wstring fileName = L"SimCity-text-" + std::wstring(reason) + L"-" +
                                      std::wstring(digestHex.begin(), digestHex.begin() + 16) +
                                      L".bin";
        outputPath = directory / fileName;
        if (std::filesystem::exists(outputPath)) {
            return true;
        }

        std::ofstream stream(outputPath, std::ios::binary | std::ios::trunc);
        if (!stream) {
            error = "Could not open runtime .text capture for writing";
            return false;
        }
        stream.write(reinterpret_cast<const char*>(bytes.data()),
                     static_cast<std::streamsize>(bytes.size()));
        if (!stream) {
            error = "Could not write the complete runtime .text capture";
            return false;
        }
        return true;
    } catch (const std::exception& exception) {
        error = exception.what();
        return false;
    } catch (...) {
        error = "Unknown exception while capturing runtime .text";
        return false;
    }
}

/** Runs the image observer until the loader lifecycle requests shutdown. */
void ObserverMain(
    const TextRegion region, const std::filesystem::path captureDirectory) noexcept {
    using logging::AsyncLogger;
    using logging::Level;
    using namespace std::chrono_literals;

    std::string digest;
    std::filesystem::path capturePath;
    std::string error;
    if (CaptureText(region, captureDirectory, L"initial", digest, capturePath, error)) {
        AsyncLogger::Instance().WriteFormat(
            Level::Info,
            "Initial loaded .text captured: RVA=0x%08X size=0x%08X SHA-256=%s path=%ls",
            region.rva, region.size, digest.c_str(), capturePath.c_str());
    } else {
        AsyncLogger::Instance().WriteFormat(
            Level::Warning, "Initial loaded .text capture failed: %s", error.c_str());
    }

    std::uint64_t stableSample = SampleFingerprint(region);
    std::uint64_t pendingSample = stableSample;
    auto lastMutation = std::chrono::steady_clock::now();
    bool mutationPending = false;
    bool firstMutationCaptured = false;
    bool unchangedNoticeWritten = false;
    std::size_t captureSequence = 0;
    const auto started = std::chrono::steady_clock::now();

    while (!g_observer.stopRequested.load(std::memory_order_acquire)) {
        const auto sampleInterval =
            std::chrono::steady_clock::now() - started < 5s ? 10ms : 250ms;
        std::this_thread::sleep_for(sampleInterval);
        const std::uint64_t sample = SampleFingerprint(region);
        const auto now = std::chrono::steady_clock::now();
        if (sample != pendingSample) {
            pendingSample = sample;
            lastMutation = now;
            mutationPending = sample != stableSample;
            if (mutationPending && captureSequence == 0) {
                AsyncLogger::Instance().Write(
                    Level::Info, "Loaded .text mutation detected; waiting for a stable snapshot");
            }
            if (mutationPending && !firstMutationCaptured) {
                error.clear();
                digest.clear();
                capturePath.clear();
                if (CaptureText(
                        region, captureDirectory, L"first-mutation", digest,
                        capturePath, error)) {
                    firstMutationCaptured = true;
                    AsyncLogger::Instance().WriteFormat(
                        Level::Info,
                        "First observed .text mutation captured immediately: SHA-256=%s path=%ls",
                        digest.c_str(), capturePath.c_str());
                } else {
                    AsyncLogger::Instance().WriteFormat(
                        Level::Warning,
                        "Immediate .text mutation capture failed: %s", error.c_str());
                }
            }
        }

        if (!unchangedNoticeWritten && now - started >= 15s) {
            unchangedNoticeWritten = true;
            if (!mutationPending && captureSequence == 0) {
                AsyncLogger::Instance().Write(
                    Level::Info,
                    "Loaded .text sample remained unchanged for 15 seconds after initialization");
            }
        }

        if (!mutationPending || now - lastMutation < 1500ms) {
            continue;
        }

        error.clear();
        digest.clear();
        capturePath.clear();
        const std::wstring reason = L"runtime-" + std::to_wstring(captureSequence + 1);
        if (CaptureText(region, captureDirectory, reason, digest, capturePath, error)) {
            ++captureSequence;
            stableSample = pendingSample;
            mutationPending = false;
            AsyncLogger::Instance().WriteFormat(
                Level::Info,
                "Stable runtime .text captured: sequence=%zu SHA-256=%s path=%ls",
                captureSequence, digest.c_str(), capturePath.c_str());
        } else {
            lastMutation = now;
            AsyncLogger::Instance().WriteFormat(
                Level::Warning, "Stable runtime .text capture failed: %s", error.c_str());
        }
    }

    g_observer.running.store(false, std::memory_order_release);
}

}  // namespace

bool StartImageObserver(
    HMODULE executableModule,
    const std::filesystem::path& captureDirectory,
    std::string& error) noexcept {
    bool expected = false;
    if (!g_observer.running.compare_exchange_strong(expected, true)) {
        return true;
    }

    TextRegion region{};
    if (!FindTextRegion(executableModule, region, error)) {
        g_observer.running.store(false, std::memory_order_release);
        return false;
    }

    try {
        g_observer.stopRequested.store(false, std::memory_order_release);
        g_observer.worker = std::thread(ObserverMain, region, captureDirectory);
        return true;
    } catch (const std::exception& exception) {
        error = exception.what();
    } catch (...) {
        error = "Unknown exception while starting the image observer";
    }
    g_observer.running.store(false, std::memory_order_release);
    return false;
}

void StopImageObserver() noexcept {
    g_observer.stopRequested.store(true, std::memory_order_release);
    if (g_observer.worker.joinable()) {
        g_observer.worker.join();
    }
    g_observer.running.store(false, std::memory_order_release);
}

}  // namespace sc13::runtime
