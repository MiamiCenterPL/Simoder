#include "mods/mod_state_store.hpp"

#include "formats/toon/toon_document.hpp"
#include "mods/mod_definition_parser.hpp"

#include <Windows.h>

#include <fstream>
#include <set>

namespace sc13::mods {

bool LoadEnabledState(
    const std::filesystem::path& path,
    std::vector<ModId>& enabled,
    std::string& error) noexcept {
    try {
        enabled.clear();
        error.clear();
        std::error_code existsError;
        const bool exists = std::filesystem::exists(path, existsError);
        if (existsError) {
            error = "Could not query state.toon";
            return false;
        }
        if (!exists) {
            return true;
        }
        formats::toon::Document document;
        formats::toon::ParseError parseError;
        if (!formats::toon::LoadFile(path, document, parseError)) {
            error = "state.toon";
            if (parseError.line != 0U) {
                error += ":" + std::to_string(parseError.line);
            }
            error += ": " + parseError.message;
            return false;
        }
        const auto field = document.root().find("enabled");
        if (field == document.root().end() || field->second.AsArray() == nullptr) {
            error = "state.toon requires an enabled array";
            return false;
        }
        std::set<ModId, std::less<>> unique;
        std::vector<ModId> parsed;
        for (const formats::toon::Value& value : *field->second.AsArray()) {
            const std::string* const id = value.AsString();
            if (id == nullptr || !IsValidModId(*id)) {
                error = "state.toon contains an invalid mod ID";
                return false;
            }
            if (!unique.insert(*id).second) {
                error = "state.toon contains a duplicate mod ID";
                return false;
            }
            parsed.push_back(*id);
        }
        enabled = std::move(parsed);
        return true;
    } catch (...) {
        enabled.clear();
        error = "Enabled-state load failed due to an allocation exception";
        return false;
    }
}

bool SaveEnabledState(
    const std::filesystem::path& path,
    std::span<const ModId> enabled,
    std::string& error) noexcept {
    try {
        error.clear();
        std::set<ModId, std::less<>> unique;
        for (const ModId& id : enabled) {
            if (!IsValidModId(id) || !unique.insert(id).second) {
                error = "Enabled-state save received an invalid or duplicate mod ID";
                return false;
            }
        }
        std::error_code directoryError;
        std::filesystem::create_directories(path.parent_path(), directoryError);
        if (directoryError) {
            error = "Could not create the state.toon parent directory";
            return false;
        }
        std::string text;
        if (enabled.empty()) {
            text = "enabled[0]:\n";
        } else {
            text = "enabled[" + std::to_string(enabled.size()) + "]: ";
            for (std::size_t index = 0U; index < enabled.size(); ++index) {
                if (index != 0U) {
                    text.push_back(',');
                }
                text += enabled[index];
            }
            text.push_back('\n');
        }
        const std::filesystem::path temporary = path.wstring() + L".tmp";
        {
            std::ofstream stream(temporary, std::ios::binary | std::ios::trunc);
            if (!stream) {
                error = "Could not create temporary state.toon";
                return false;
            }
            stream.write(text.data(), static_cast<std::streamsize>(text.size()));
            stream.flush();
            if (!stream) {
                error = "Could not write complete temporary state.toon";
                return false;
            }
        }
        if (MoveFileExW(
                temporary.c_str(), path.c_str(),
                MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) == FALSE) {
            const DWORD moveError = GetLastError();
            DeleteFileW(temporary.c_str());
            error = "Could not atomically publish state.toon; Win32 error " +
                    std::to_string(moveError);
            return false;
        }
        return true;
    } catch (...) {
        error = "Enabled-state save failed due to an allocation exception";
        return false;
    }
}

}  // namespace sc13::mods
