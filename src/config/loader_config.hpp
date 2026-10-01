#pragma once

#include <filesystem>
#include <string>

namespace sc13::config {

/** Configures the optional centralized developer-console sink. */
struct DeveloperConsoleConfig final {
    bool enabled{};
    bool captureGameLogs{true};
    bool captureLoaderLogs{true};
    bool captureModLogs{true};
    std::string level{"info"};
};

/** Stores typed process-start configuration with fail-safe defaults. */
struct LoaderConfig final {
    DeveloperConsoleConfig developerConsole;
};

/** Loads config.toon or creates the documented default when it is absent. */
[[nodiscard]] bool LoadOrCreateConfig(
    const std::filesystem::path& simoderDirectory,
    LoaderConfig& config,
    std::string& error) noexcept;

/** Returns the canonical default configuration text. */
[[nodiscard]] std::string DefaultConfigText();

}  // namespace sc13::config
