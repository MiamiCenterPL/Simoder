#include "logging/async_logger.hpp"

#include <algorithm>
#include <cstdio>
#include <new>

namespace sc13::logging {
namespace {

/** Formats one semantic event identically for all text sinks. */
template <typename Record>
[[nodiscard]] std::size_t FormatRecord(
    const Record& record,
    char* destination,
    std::size_t capacity) noexcept {
    const int written = std::snprintf(
        destination, capacity,
        "[%04u-%02u-%02u %02u:%02u:%02u.%03u] [%s] [%s] [%s] [T%lu] %s\n",
        record.time.wYear, record.time.wMonth, record.time.wDay,
        record.time.wHour, record.time.wMinute, record.time.wSecond,
        record.time.wMilliseconds, SourceTypeName(record.source),
        record.sourceName.data(), LevelName(record.level), record.threadId,
        record.message.data());
    if (written <= 0) {
        return 0U;
    }
    return (std::min)(static_cast<std::size_t>(written), capacity - 1U);
}

}  // namespace

AsyncLogger& AsyncLogger::Instance() noexcept {
    alignas(AsyncLogger) static std::byte storage[sizeof(AsyncLogger)];
    static AsyncLogger* instance = ::new (static_cast<void*>(storage)) AsyncLogger();
    return *instance;
}

AsyncLogger::~AsyncLogger() {
    Stop();
}

bool AsyncLogger::Start(
    const std::filesystem::path& path,
    const DeveloperConsoleOptions& console) noexcept {
    try {
        const std::filesystem::path parent = path.parent_path();
        if (!parent.empty()) {
            std::error_code directoryError;
            std::filesystem::create_directories(parent, directoryError);
            if (directoryError) {
                return false;
            }
        }

        AcquireSRWLockExclusive(&lock_);
        if (running_) {
            ReleaseSRWLockExclusive(&lock_);
            return true;
        }
        stream_.open(path, std::ios::out | std::ios::app);
        if (!stream_) {
            ReleaseSRWLockExclusive(&lock_);
            return false;
        }
        consoleOptions_ = console;
        if (console.enabled && AllocConsole() != FALSE) {
            consoleAllocated_ = true;
            SetConsoleTitleW(L"Simoder Developer Console");
            consoleOutput_ = CreateFileW(
                L"CONOUT$", GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (consoleOutput_ == INVALID_HANDLE_VALUE) {
                FreeConsole();
                consoleAllocated_ = false;
            }
        }
        readIndex_ = 0U;
        writeIndex_ = 0U;
        count_ = 0U;
        dropped_ = 0U;
        stopping_ = false;
        running_ = true;
        ReleaseSRWLockExclusive(&lock_);

        worker_ = std::thread(&AsyncLogger::WorkerMain, this);
        return true;
    } catch (...) {
        AcquireSRWLockExclusive(&lock_);
        running_ = false;
        stopping_ = true;
        if (consoleOutput_ != INVALID_HANDLE_VALUE) {
            CloseHandle(consoleOutput_);
            consoleOutput_ = INVALID_HANDLE_VALUE;
        }
        if (consoleAllocated_) {
            FreeConsole();
            consoleAllocated_ = false;
        }
        if (stream_.is_open()) {
            stream_.close();
        }
        ReleaseSRWLockExclusive(&lock_);
        return false;
    }
}

void AsyncLogger::Stop() noexcept {
    try {
        AcquireSRWLockExclusive(&lock_);
        if (!running_) {
            ReleaseSRWLockExclusive(&lock_);
            return;
        }
        stopping_ = true;
        WakeAllConditionVariable(&wake_);
        ReleaseSRWLockExclusive(&lock_);

        if (worker_.joinable()) {
            worker_.join();
        }

        AcquireSRWLockExclusive(&lock_);
        running_ = false;
        if (stream_.is_open()) {
            stream_.flush();
            stream_.close();
        }
        if (consoleOutput_ != INVALID_HANDLE_VALUE) {
            CloseHandle(consoleOutput_);
            consoleOutput_ = INVALID_HANDLE_VALUE;
        }
        if (consoleAllocated_) {
            FreeConsole();
            consoleAllocated_ = false;
        }
        ReleaseSRWLockExclusive(&lock_);
    } catch (...) {
    }
}

bool AsyncLogger::IsConsoleActive() const noexcept {
    AcquireSRWLockShared(&lock_);
    const bool active = consoleAllocated_ && consoleOutput_ != INVALID_HANDLE_VALUE;
    ReleaseSRWLockShared(&lock_);
    return active;
}

void AsyncLogger::Write(Level level, const char* message) noexcept {
    WriteEvent(level, SourceType::Loader, "SC13ModLoader", message);
}

void AsyncLogger::WriteFormat(Level level, const char* format, ...) noexcept {
    if (format == nullptr) {
        return;
    }
    std::array<char, kMessageCapacity> buffer{};
    va_list arguments;
    va_start(arguments, format);
    _vsnprintf_s(buffer.data(), buffer.size(), _TRUNCATE, format, arguments);
    va_end(arguments);
    Write(level, buffer.data());
}

void AsyncLogger::WriteEvent(
    Level level,
    SourceType source,
    const char* sourceName,
    const char* message) noexcept {
    if (message == nullptr) {
        return;
    }
    AcquireSRWLockExclusive(&lock_);
    if (!running_ || stopping_) {
        ReleaseSRWLockExclusive(&lock_);
        return;
    }
    if (count_ == kCapacity) {
        ++dropped_;
        ReleaseSRWLockExclusive(&lock_);
        return;
    }
    Record& record = records_[writeIndex_];
    GetLocalTime(&record.time);
    record.threadId = GetCurrentThreadId();
    record.level = level;
    record.source = source;
    strncpy_s(
        record.sourceName.data(), record.sourceName.size(),
        sourceName == nullptr || *sourceName == '\0' ? "-" : sourceName, _TRUNCATE);
    strncpy_s(record.message.data(), record.message.size(), message, _TRUNCATE);
    writeIndex_ = (writeIndex_ + 1U) % kCapacity;
    ++count_;
    WakeConditionVariable(&wake_);
    ReleaseSRWLockExclusive(&lock_);
}

void AsyncLogger::WriteEventFormat(
    Level level,
    SourceType source,
    const char* sourceName,
    const char* format,
    ...) noexcept {
    if (format == nullptr) {
        return;
    }
    std::array<char, kMessageCapacity> buffer{};
    va_list arguments;
    va_start(arguments, format);
    _vsnprintf_s(buffer.data(), buffer.size(), _TRUNCATE, format, arguments);
    va_end(arguments);
    WriteEvent(level, source, sourceName, buffer.data());
}

bool AsyncLogger::ShouldWriteConsole(const Record& record) const noexcept {
    const bool sourceEnabled =
        (record.source == SourceType::Game && consoleOptions_.captureGameLogs) ||
        (record.source == SourceType::Loader && consoleOptions_.captureLoaderLogs) ||
        (record.source == SourceType::Mod && consoleOptions_.captureModLogs);
    return sourceEnabled && PassesMinimum(record.level, consoleOptions_.minimumLevel);
}

void AsyncLogger::WorkerMain() noexcept {
    for (;;) {
        std::array<Record, kBatchCapacity> batch{};
        std::size_t batchSize = 0U;
        std::uint64_t dropped = 0U;
        bool stopping = false;
        AcquireSRWLockExclusive(&lock_);
        while (count_ == 0U && !stopping_) {
            SleepConditionVariableSRW(&wake_, &lock_, INFINITE, 0);
        }
        batchSize = (std::min)(count_, batch.size());
        for (std::size_t index = 0U; index < batchSize; ++index) {
            batch[index] = records_[readIndex_];
            readIndex_ = (readIndex_ + 1U) % kCapacity;
        }
        count_ -= batchSize;
        dropped = dropped_;
        dropped_ = 0U;
        stopping = stopping_ && count_ == 0U;
        ReleaseSRWLockExclusive(&lock_);

        std::array<char, kFormattedCapacity> formatted{};
        for (std::size_t index = 0U; index < batchSize; ++index) {
            const std::size_t length = FormatRecord(
                batch[index], formatted.data(), formatted.size());
            if (length == 0U) {
                continue;
            }
            if (stream_) {
                stream_.write(formatted.data(), static_cast<std::streamsize>(length));
            }
            if (consoleOutput_ != INVALID_HANDLE_VALUE &&
                ShouldWriteConsole(batch[index])) {
                DWORD bytesWritten = 0U;
                WriteFile(
                    consoleOutput_, formatted.data(), static_cast<DWORD>(length),
                    &bytesWritten, nullptr);
            }
        }
        if (dropped != 0U) {
            const int length = std::snprintf(
                formatted.data(), formatted.size(),
                "[LOADER] [WARN] Dropped %llu log events because the queue was full.\n",
                static_cast<unsigned long long>(dropped));
            if (length > 0 && stream_) {
                stream_.write(formatted.data(), length);
            }
        }
        if (stream_) {
            stream_.flush();
        }
        if (stopping) {
            return;
        }
    }
}

}  // namespace sc13::logging
