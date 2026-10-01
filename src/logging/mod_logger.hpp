#pragma once

#include "logging/log_event.hpp"

#include <string>

namespace sc13::logging {

/** Tags framework and future code-mod events with one stable mod identity. */
class ModLogger final {
public:
    /** Creates a producer bound to one validated mod ID. */
    explicit ModLogger(std::string modId);

    /** Emits an informational mod event. */
    void Info(const char* message) const noexcept;

    /** Emits a warning mod event. */
    void Warning(const char* message) const noexcept;

    /** Emits an error mod event. */
    void Error(const char* message) const noexcept;

private:
    std::string modId_;
};

}  // namespace sc13::logging
