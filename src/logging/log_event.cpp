#include "logging/log_event.hpp"

namespace sc13::logging {
namespace {

/** Maps a level to its filtering rank while keeping Patch equivalent to Info. */
[[nodiscard]] unsigned int LevelRank(Level level) noexcept {
    switch (level) {
        case Level::Trace: return 0U;
        case Level::Debug: return 1U;
        case Level::Info:
        case Level::Patch: return 2U;
        case Level::Warning: return 3U;
        case Level::Error: return 4U;
    }
    return 4U;
}

}  // namespace

const char* LevelName(Level level) noexcept {
    switch (level) {
        case Level::Trace: return "TRACE";
        case Level::Debug: return "DEBUG";
        case Level::Info: return "INFO";
        case Level::Warning: return "WARN";
        case Level::Error: return "ERROR";
        case Level::Patch: return "PATCH";
    }
    return "UNKNOWN";
}

const char* SourceTypeName(SourceType source) noexcept {
    switch (source) {
        case SourceType::Game: return "GAME";
        case SourceType::Loader: return "LOADER";
        case SourceType::Mod: return "MOD";
    }
    return "UNKNOWN";
}

bool ParseLevel(std::string_view text, Level& level) noexcept {
    if (text == "trace") {
        level = Level::Trace;
    } else if (text == "debug") {
        level = Level::Debug;
    } else if (text == "info") {
        level = Level::Info;
    } else if (text == "warning" || text == "warn") {
        level = Level::Warning;
    } else if (text == "error") {
        level = Level::Error;
    } else {
        return false;
    }
    return true;
}

bool PassesMinimum(Level level, Level minimum) noexcept {
    return LevelRank(level) >= LevelRank(minimum);
}

}  // namespace sc13::logging
