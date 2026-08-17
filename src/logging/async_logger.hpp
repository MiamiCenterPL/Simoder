#pragma once

#include <Windows.h>

#include <array>
#include <atomic>
#include <cstdarg>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <thread>

namespace sc13::logging {

/** Defines the stable severity labels written to the loader log. */
enum class Level {
    Trace,
    Info,
    Warning,
    Error,
    Patch,
};

/** Implements a bounded asynchronous logger suitable for filtered hook events. */
class AsyncLogger final {
public:
    /** Returns the process-wide logger instance. */
    [[nodiscard]] static AsyncLogger& Instance() noexcept;

    /** Opens the log and starts its worker thread. */
    [[nodiscard]] bool Start(const std::filesystem::path& path) noexcept;

    /** Flushes queued records and stops the worker thread. */
    void Stop() noexcept;

    /** Enqueues an already formatted message without throwing. */
    void Write(Level level, const char* message) noexcept;

    /** Formats and enqueues a bounded message without throwing. */
    void WriteFormat(Level level, const char* format, ...) noexcept;

private:
    static constexpr std::size_t kCapacity = 1024;
    static constexpr std::size_t kMessageCapacity = 1536;

    /** Stores one fixed-size record without allocating in a hook. */
    struct Record {
        SYSTEMTIME time{};
        DWORD threadId{};
        Level level{};
        std::array<char, kMessageCapacity> message{};
    };

    AsyncLogger() = default;
    ~AsyncLogger();
    AsyncLogger(const AsyncLogger&) = delete;
    AsyncLogger& operator=(const AsyncLogger&) = delete;

    /** Drains queued records until Stop requests termination. */
    void WorkerMain() noexcept;

    SRWLOCK lock_ = SRWLOCK_INIT;
    CONDITION_VARIABLE wake_ = CONDITION_VARIABLE_INIT;
    std::array<Record, kCapacity> records_{};
    std::size_t readIndex_{};
    std::size_t writeIndex_{};
    std::size_t count_{};
    std::uint64_t dropped_{};
    bool running_{};
    bool stopping_{};
    std::ofstream stream_;
    std::thread worker_;
};

}  // namespace sc13::logging
