#pragma once

#include <string_view>

namespace sc13::logging {

/** Defines stable severities shared by all producers and sinks. */
enum class Level {
    Trace,
    Debug,
    Info,
    Warning,
    Error,
    Patch,
};

/** Classifies the semantic origin of a centralized log event. */
enum class SourceType {
    Game,
    Loader,
    Mod,
};

/** Returns the stable uppercase severity label used by every sink. */
[[nodiscard]] const char* LevelName(Level level) noexcept;

/** Returns the stable uppercase source label used by every sink. */
[[nodiscard]] const char* SourceTypeName(SourceType source) noexcept;

/** Parses one configured minimum severity name. */
[[nodiscard]] bool ParseLevel(std::string_view text, Level& level) noexcept;

/** Reports whether a severity passes a configured minimum threshold. */
[[nodiscard]] bool PassesMinimum(Level level, Level minimum) noexcept;

}  // namespace sc13::logging
