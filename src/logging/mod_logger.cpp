#include "logging/mod_logger.hpp"

#include "logging/async_logger.hpp"

#include <utility>

namespace sc13::logging {

ModLogger::ModLogger(std::string modId) : modId_(std::move(modId)) {}

void ModLogger::Info(const char* message) const noexcept {
    AsyncLogger::Instance().WriteEvent(
        Level::Info, SourceType::Mod, modId_.c_str(), message);
}

void ModLogger::Warning(const char* message) const noexcept {
    AsyncLogger::Instance().WriteEvent(
        Level::Warning, SourceType::Mod, modId_.c_str(), message);
}

void ModLogger::Error(const char* message) const noexcept {
    AsyncLogger::Instance().WriteEvent(
        Level::Error, SourceType::Mod, modId_.c_str(), message);
}

}  // namespace sc13::logging
