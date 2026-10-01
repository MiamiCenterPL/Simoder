#include "core/json.hpp"
#include "dev/mod_validation.hpp"
#include "mods/mod_definition_parser.hpp"
#include <fstream>
#include <iostream>

namespace {
/** @summary Prints the complete developer CLI contract. */
void Usage() {
    std::cerr << "simoder-dev init <mod-id> <new-directory>\n"
        "simoder-dev explain <mod> [--with <mod>] [--json]\n"
        "simoder-dev symbols <mod> [name-or-id-query] (JSON completion catalog)\n"
        "simoder-dev validate|dry-run <mod> --game <package-directory> [--with <mod>] [--json]\n";
}
/** @summary Creates a self-contained valid inactive mod scaffold with an explicit sample alias. */
int Initialize(std::wstring_view id, const std::filesystem::path& directory) {
    const std::string narrowId = sc13::core::Utf8Text(std::filesystem::path(id).u8string());
    if (!sc13::mods::IsValidModId(narrowId)) { std::cerr << "Invalid mod ID\n"; return 2; }
    if (!std::filesystem::create_directory(directory)) { std::cerr << "Output directory already exists\n"; return 1; }
    std::ofstream manifest(directory / "mod.toon", std::ios::binary);
    manifest << "id: " << narrowId << "\nname: " << narrowId << "\nversion: 1.0.0\n";
    std::ofstream overrides(directory / "overrides.toon", std::ios::binary);
    overrides << "symbols:\n  properties[1]{name,id,source}:\n"
        "    sanitizerOperatingCost,0x09AE19D7,Simoder sample alias - verified cost effect\n"
        "patches[1]:\n  - target:\n      type: 0x00B1B104\n      group: 0x61EFC000\n      instance: 0x719436BD\n"
        "    properties[1]{name,type,operation,value}:\n      sanitizerOperatingCost,float,set,345\n";
    manifest.close(); overrides.close();
    if (!manifest || !overrides) { std::cerr << "Cannot write scaffold\n"; return 1; }
    std::cout << "Created sample mod (not installed or enabled): " << narrowId << '\n';
    return 0;
}
}

/** @summary Runs deterministic offline validation without modifying packages or game state. */
int wmain(int argc, wchar_t** argv) {
    try {
        if (argc == 4 && std::wstring_view(argv[1]) == L"init") return Initialize(argv[2], argv[3]);
        if ((argc == 3 || argc == 4) && std::wstring_view(argv[1]) == L"symbols") {
            std::string json, error;
            const auto query = argc == 4 ? sc13::core::Utf8Text(std::filesystem::path(argv[3]).u8string()) : std::string{};
            if (!sc13::mods::ListSymbolsJson(argv[2], query, json, error)) { std::cerr << error << '\n'; return 1; }
            std::cout << json; return 0;
        }
        if (argc < 3) { Usage(); return 2; }
        const std::wstring_view command(argv[1]);
        if (command != L"explain" && command != L"validate" && command != L"dry-run") { Usage(); return 2; }
        std::vector<std::filesystem::path> directories{argv[2]};
        std::filesystem::path game;
        bool json = false;
        for (int index = 3; index < argc; ++index) {
            const std::wstring_view option(argv[index]);
            if (option == L"--json") json = true;
            else if ((option == L"--game" || option == L"--with") && index + 1 < argc) {
                const auto path = std::filesystem::path(argv[++index]);
                if (option == L"--game") game = path; else directories.push_back(path);
            } else { Usage(); return 2; }
        }
        if (command != L"explain" && game.empty()) { std::cerr << "--game is required\n"; return 2; }
        std::vector<sc13::mods::ModDefinition> definitions;
        for (const auto& directory : directories) {
            sc13::mods::ModDefinition definition;
            std::string error;
            if (!sc13::mods::LoadModDefinition(directory, definition, error)) {
                sc13::dev::ValidationReport failed;
                failed.valid = false; failed.errors.push_back(error);
                std::cout << (json ? sc13::dev::ValidationJson(failed) : sc13::dev::ValidationText(failed));
                return 1;
            }
            definitions.push_back(std::move(definition));
        }
        const auto report = sc13::dev::ValidateMods(definitions, game);
        std::cout << (json ? sc13::dev::ValidationJson(report) : sc13::dev::ValidationText(report));
        return report.valid ? 0 : 1;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
