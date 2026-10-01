#include "config/loader_config.hpp"

#include "formats/toon/toon_document.hpp"

#include <fstream>
#include <string_view>

namespace sc13::config {
namespace {

/** Reads one optional boolean while preserving its caller-provided default. */
[[nodiscard]] bool ReadOptionalBoolean(
    const formats::toon::Value& object,
    std::string_view key,
    bool& value,
    std::string& error) {
    const formats::toon::Value* const field = object.Find(key);
    if (field == nullptr) {
        return true;
    }
    const bool* const boolean = field->AsBoolean();
    if (boolean == nullptr) {
        error = "developerConsole." + std::string(key) + " must be boolean";
        return false;
    }
    value = *boolean;
    return true;
}

/** Writes a complete UTF-8 file through a sibling temporary and atomic rename. */
[[nodiscard]] bool WriteAtomic(
    const std::filesystem::path& path,
    std::string_view text,
    std::string& error) {
    const std::filesystem::path temporary = path.wstring() + L".tmp";
    {
        std::ofstream stream(temporary, std::ios::binary | std::ios::trunc);
        if (!stream) {
            error = "Could not create temporary config file";
            return false;
        }
        stream.write(text.data(), static_cast<std::streamsize>(text.size()));
        stream.flush();
        if (!stream) {
            error = "Could not write complete temporary config file";
            return false;
        }
    }
    std::error_code renameError;
    std::filesystem::rename(temporary, path, renameError);
    if (renameError) {
        std::error_code removeError;
        std::filesystem::remove(path, removeError);
        renameError.clear();
        std::filesystem::rename(temporary, path, renameError);
    }
    if (renameError) {
        std::error_code cleanupError;
        std::filesystem::remove(temporary, cleanupError);
        error = "Could not atomically publish config.toon";
        return false;
    }
    return true;
}

}  // namespace

std::string DefaultConfigText() {
    return
        "developerConsole:\n"
        "  enabled: false\n"
        "  captureGameLogs: true\n"
        "  captureLoaderLogs: true\n"
        "  captureModLogs: true\n"
        "  level: info\n";
}

bool LoadOrCreateConfig(
    const std::filesystem::path& simoderDirectory,
    LoaderConfig& config,
    std::string& error) noexcept {
    try {
        config = {};
        error.clear();
        std::error_code directoryError;
        std::filesystem::create_directories(simoderDirectory, directoryError);
        if (directoryError) {
            error = "Could not create the simoder configuration directory";
            return false;
        }
        const std::filesystem::path path = simoderDirectory / L"config.toon";
        if (!std::filesystem::exists(path)) {
            if (!WriteAtomic(path, DefaultConfigText(), error)) {
                return false;
            }
        }

        formats::toon::Document document;
        formats::toon::ParseError parseError;
        if (!formats::toon::LoadFile(path, document, parseError)) {
            error = "config.toon";
            if (parseError.line != 0U) {
                error += ":" + std::to_string(parseError.line);
            }
            error += ": " + parseError.message;
            return false;
        }
        const auto console = document.root().find("developerConsole");
        if (console == document.root().end() || console->second.AsObject() == nullptr) {
            error = "config.toon requires a developerConsole object";
            return false;
        }
        DeveloperConsoleConfig parsed;
        if (!ReadOptionalBoolean(
                console->second, "enabled", parsed.enabled, error) ||
            !ReadOptionalBoolean(
                console->second, "captureGameLogs", parsed.captureGameLogs, error) ||
            !ReadOptionalBoolean(
                console->second, "captureLoaderLogs", parsed.captureLoaderLogs, error) ||
            !ReadOptionalBoolean(
                console->second, "captureModLogs", parsed.captureModLogs, error)) {
            return false;
        }
        if (const formats::toon::Value* level = console->second.Find("level");
            level != nullptr) {
            const std::string* const text = level->AsString();
            if (text == nullptr ||
                (*text != "trace" && *text != "debug" && *text != "info" &&
                 *text != "warn" && *text != "error")) {
                error = "developerConsole.level must be trace, debug, info, warn, or error";
                return false;
            }
            parsed.level = *text;
        }
        config.developerConsole = std::move(parsed);
        return true;
    } catch (...) {
        config = {};
        error = "Loader configuration failed due to an allocation exception";
        return false;
    }
}

}  // namespace sc13::config
