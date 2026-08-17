#include "logging/async_logger.hpp"

#include <cstdio>
#include <new>

namespace sc13::logging {
namespace {

/** Converts a severity enum to its stable log label. */
[[nodiscard]] const char* LevelName(Level level) noexcept {
    switch (level) {
        case Level::Trace: return "TRACE";
        case Level::Info: return "INFO";
        case Level::Warning: return "WARN";
        case Level::Error: return "ERROR";
        case Level::Patch: return "PATCH";
    }
    return "UNKNOWN";
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

bool AsyncLogger::Start(const std::filesystem::path& path) noexcept {
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
        readIndex_ = 0;
        writeIndex_ = 0;
        count_ = 0;
        dropped_ = 0;
        stopping_ = false;
        running_ = true;
        ReleaseSRWLockExclusive(&lock_);

        worker_ = std::thread(&AsyncLogger::WorkerMain, this);
        return true;
    } catch (...) {
        AcquireSRWLockExclusive(&lock_);
        running_ = false;
        stopping_ = true;
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
        ReleaseSRWLockExclusive(&lock_);
    } catch (...) {
    }
}

void AsyncLogger::Write(Level level, const char* message) noexcept {
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
    strncpy_s(record.message.data(), record.message.size(), message, _TRUNCATE);
    writeIndex_ = (writeIndex_ + 1U) % kCapacity;
    ++count_;
    WakeConditionVariable(&wake_);
    ReleaseSRWLockExclusive(&lock_);
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

void AsyncLogger::WorkerMain() noexcept {
    for (;;) {
        Record record{};
        std::uint64_t dropped = 0;
        AcquireSRWLockExclusive(&lock_);
        while (count_ == 0 && !stopping_) {
            SleepConditionVariableSRW(&wake_, &lock_, INFINITE, 0);
        }
        if (count_ == 0 && stopping_) {
            dropped = dropped_;
            dropped_ = 0;
            ReleaseSRWLockExclusive(&lock_);
            if (dropped != 0 && stream_) {
                stream_ << "[WARN] Dropped " << dropped << " log records because the queue was full.\n";
            }
            if (stream_) {
                stream_.flush();
            }
            return;
        }
        record = records_[readIndex_];
        readIndex_ = (readIndex_ + 1U) % kCapacity;
        --count_;
        ReleaseSRWLockExclusive(&lock_);

        if (stream_) {
            char timestamp[64]{};
            std::snprintf(
                timestamp, sizeof(timestamp), "%04u-%02u-%02u %02u:%02u:%02u.%03u",
                record.time.wYear, record.time.wMonth, record.time.wDay, record.time.wHour,
                record.time.wMinute, record.time.wSecond, record.time.wMilliseconds);
            stream_ << '[' << timestamp << "] [" << LevelName(record.level) << "] [T"
                    << record.threadId << "] " << record.message.data() << '\n';
            stream_.flush();
        }
    }
}

}  // namespace sc13::logging
