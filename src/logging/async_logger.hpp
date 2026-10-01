#pragma once

#include "logging/log_event.hpp"

#include <Windows.h>

#include <array>
#include <cstdarg>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <thread>

namespace sc13::logging {

/** Configures the optional console sink independently from event producers. */
struct DeveloperConsoleOptions final {
    bool enabled{};
    bool captureGameLogs{true};
    bool captureLoaderLogs{true};
    bool captureModLogs{true};
    Level minimumLevel{Level::Info};
};

/** Implements one bounded central event queue with file and optional console sinks. */
class AsyncLogger final {
public:
    /** Returns the process-wide logger instance. */
    [[nodiscard]] static AsyncLogger& Instance() noexcept;

    /** Opens the file sink and optionally creates the independent console sink. */
    [[nodiscard]] bool Start(
        const std::filesystem::path& path,
        const DeveloperConsoleOptions& console = {}) noexcept;

    /** Flushes queued events, closes sinks, and stops the worker thread. */
    void Stop() noexcept;

    /** Reports whether the optional console sink was created successfully. */
    [[nodiscard]] bool IsConsoleActive() const noexcept;

    /** Enqueues a loader event for compatibility with existing producers. */
    void Write(Level level, const char* message) noexcept;

    /** Formats and enqueues a loader event for compatibility with existing producers. */
    void WriteFormat(Level level, const char* format, ...) noexcept;

    /** Enqueues one semantically classified event without throwing. */
    void WriteEvent(
        Level level,
        SourceType source,
        const char* sourceName,
        const char* message) noexcept;

    /** Formats and enqueues one semantically classified event without throwing. */
    void WriteEventFormat(
        Level level,
        SourceType source,
        const char* sourceName,
        const char* format,
        ...) noexcept;

private:
    static constexpr std::size_t kCapacity = 2048U;
    static constexpr std::size_t kBatchCapacity = 64U;
    static constexpr std::size_t kSourceNameCapacity = 160U;
    static constexpr std::size_t kMessageCapacity = 1536U;
    static constexpr std::size_t kFormattedCapacity = 2048U;

    /** Stores one fixed-size semantic event without allocating in a hook. */
    struct Record final {
        SYSTEMTIME time{};
        DWORD threadId{};
        Level level{};
        SourceType source{SourceType::Loader};
        std::array<char, kSourceNameCapacity> sourceName{};
        std::array<char, kMessageCapacity> message{};
    };

    AsyncLogger() = default;
    ~AsyncLogger();
    AsyncLogger(const AsyncLogger&) = delete;
    AsyncLogger& operator=(const AsyncLogger&) = delete;

    /** Drains bounded batches until Stop requests termination. */
    void WorkerMain() noexcept;

    /** Reports whether one event is enabled for the optional console sink. */
    [[nodiscard]] bool ShouldWriteConsole(const Record& record) const noexcept;

    mutable SRWLOCK lock_ = SRWLOCK_INIT;
    CONDITION_VARIABLE wake_ = CONDITION_VARIABLE_INIT;
    std::array<Record, kCapacity> records_{};
    std::size_t readIndex_{};
    std::size_t writeIndex_{};
    std::size_t count_{};
    std::uint64_t dropped_{};
    bool running_{};
    bool stopping_{};
    bool consoleAllocated_{};
    HANDLE consoleOutput_{INVALID_HANDLE_VALUE};
    DeveloperConsoleOptions consoleOptions_{};
    std::ofstream stream_;
    std::thread worker_;
};

}  // namespace sc13::logging
