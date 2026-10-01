#include "bootstrap/loader.hpp"

#include "config/loader_config.hpp"
#include "core/sha256.hpp"
#include "hooks/file_trace.hpp"
#include "hooks/resource_trace.hpp"
#include "logging/async_logger.hpp"
#include "mods/mod_manager.hpp"
#include "mods/patch_registry.hpp"
#include "reverse/game_build.hpp"
#if defined(SC13_ENABLE_DISCOVERY_TRACE)
#include "runtime/image_observer.hpp"
#endif
#include "runtime/patch_signal.hpp"
#include "runtime/runtime_resource_cache.hpp"
#include "ui/simoder_overlay.hpp"

#include <Windows.h>

#include <array>
#include <atomic>
#include <filesystem>
#include <memory>
#include <set>
#include <string>
#include <vector>

namespace sc13::bootstrap {
namespace {

HMODULE g_module = nullptr;
std::atomic_bool g_initialized = false;
std::unique_ptr<mods::PatchRegistry> g_patchRegistry;
std::unique_ptr<runtime::RuntimeResourceCache> g_resourceCache;
std::unique_ptr<mods::ModManager> g_modManager;
config::LoaderConfig g_loaderConfig;

/** Resolves the injected DLL path for colocated logs and diagnostics. */
[[nodiscard]] bool ReadModulePath(std::filesystem::path& path) noexcept {
    std::array<wchar_t, 32768> buffer{};
    const DWORD length = GetModuleFileNameW(g_module, buffer.data(), static_cast<DWORD>(buffer.size()));
    if (length == 0 || length == buffer.size()) {
        return false;
    }
    path.assign(buffer.data(), buffer.data() + length);
    return true;
}

/** Returns true only when initialization is running inside SimCity.exe. */
[[nodiscard]] bool IsSimCityProcess(const std::filesystem::path& path) noexcept {
    return _wcsicmp(path.filename().c_str(), L"SimCity.exe") == 0;
}

/** Releases high-level mod services after all hooks have stopped using them. */
void ResetModServices() noexcept {
    g_modManager.reset();
    g_resourceCache.reset();
    g_patchRegistry.reset();
    g_loaderConfig = {};
}

}  // namespace

void SetModuleHandle(HMODULE module) noexcept {
    g_module = module;
}

}  // namespace sc13::bootstrap

extern "C" DWORD WINAPI SC13_Initialize(void*) noexcept {
    using sc13::logging::AsyncLogger;
    using sc13::logging::Level;

    bool expected = false;
    if (!sc13::bootstrap::g_initialized.compare_exchange_strong(expected, true)) {
        return 0;
    }

    try {
        std::filesystem::path dllPath;
        if (!sc13::bootstrap::ReadModulePath(dllPath)) {
            sc13::bootstrap::g_initialized.store(false);
            return 10;
        }
        const std::filesystem::path simoderDirectory = dllPath.parent_path();
        const std::filesystem::path gameRoot = simoderDirectory.parent_path();
        const std::filesystem::path modsDirectory = gameRoot / L"mods";
        const std::filesystem::path logPath =
            simoderDirectory / L"logs" / L"sc13modloader.log";
        std::string error;
        const bool configValid = sc13::config::LoadOrCreateConfig(
            simoderDirectory, sc13::bootstrap::g_loaderConfig, error);
        sc13::logging::DeveloperConsoleOptions console;
        console.enabled = sc13::bootstrap::g_loaderConfig.developerConsole.enabled;
        console.captureGameLogs =
            sc13::bootstrap::g_loaderConfig.developerConsole.captureGameLogs;
        console.captureLoaderLogs =
            sc13::bootstrap::g_loaderConfig.developerConsole.captureLoaderLogs;
        console.captureModLogs =
            sc13::bootstrap::g_loaderConfig.developerConsole.captureModLogs;
        const bool levelValid = sc13::logging::ParseLevel(
            sc13::bootstrap::g_loaderConfig.developerConsole.level,
            console.minimumLevel);
        static_cast<void>(levelValid);
        if (!AsyncLogger::Instance().Start(logPath, console)) {
            sc13::bootstrap::g_initialized.store(false);
            return 11;
        }

        AsyncLogger::Instance().Write(Level::Info, "SC13 Mod Loader initializing");
        AsyncLogger::Instance().WriteFormat(Level::Info, "Loader DLL: %ls", dllPath.c_str());
        AsyncLogger::Instance().WriteFormat(Level::Info, "Log file: %ls", logPath.c_str());

        if (!configValid) {
            AsyncLogger::Instance().WriteFormat(
                Level::Warning,
                "Loader config rejected; safe defaults are active: %s", error.c_str());
        }
        if (console.enabled && !AsyncLogger::Instance().IsConsoleActive()) {
            AsyncLogger::Instance().Write(
                Level::Warning,
                "DeveloperConsole was requested but could not be created; file logging continues");
        }
        AsyncLogger::Instance().WriteFormat(
            Level::Info, "Game root: %ls", gameRoot.c_str());
        AsyncLogger::Instance().WriteFormat(
            Level::Info, "Mods directory: %ls", modsDirectory.c_str());

        if (!sc13::runtime::StartPatchAppliedSignal(error)) {
            AsyncLogger::Instance().WriteFormat(
                Level::Error, "Patch-applied signal setup failed: %s", error.c_str());
            return 16;
        }

        sc13::reverse::BuildFingerprint fingerprint{};
        error.clear();
        if (!sc13::reverse::InspectCurrentBuild(fingerprint, error)) {
            AsyncLogger::Instance().WriteFormat(Level::Error, "Build fingerprint failed: %s", error.c_str());
            AsyncLogger::Instance().Write(
                Level::Warning, "No hooks installed; the game is allowed to continue unchanged");
            return 12;
        }
        if (!sc13::bootstrap::IsSimCityProcess(fingerprint.executablePath)) {
            AsyncLogger::Instance().WriteFormat(
                Level::Error, "Refusing to instrument non-SimCity process: %ls",
                fingerprint.executablePath.c_str());
            return 13;
        }

        AsyncLogger::Instance().WriteFormat(
            Level::Info, "Game executable: %ls", fingerprint.executablePath.c_str());
        AsyncLogger::Instance().WriteFormat(
            Level::Info, "Game version: %s machine=0x%04X timestamp=0x%08X imageSize=0x%08X",
            fingerprint.fileVersion.c_str(), fingerprint.machine, fingerprint.timestamp,
            fingerprint.imageSize);
        AsyncLogger::Instance().WriteFormat(
            Level::Info, "Executable SHA-256: %s",
            sc13::core::ToHex(fingerprint.fileSha256).c_str());
        AsyncLogger::Instance().WriteFormat(
            Level::Info, "Loaded .text: RVA=0x%08X size=0x%08X SHA-256=%s",
            fingerprint.textRva, fingerprint.textSize,
            sc13::core::ToHex(fingerprint.loadedTextSha256).c_str());

        sc13::bootstrap::g_patchRegistry =
            std::make_unique<sc13::mods::PatchRegistry>();
        sc13::bootstrap::g_resourceCache =
            std::make_unique<sc13::runtime::RuntimeResourceCache>();
        sc13::hooks::ConfigureResourceRuntime(
            *sc13::bootstrap::g_patchRegistry,
            *sc13::bootstrap::g_resourceCache);
        sc13::bootstrap::g_modManager = std::make_unique<sc13::mods::ModManager>(
            modsDirectory, simoderDirectory / L"state.toon",
            *sc13::bootstrap::g_patchRegistry,
            *sc13::bootstrap::g_resourceCache,
            [](const std::set<sc13::core::Tgi>& affected, std::string& refreshError) {
                return sc13::hooks::RefreshRuntimeResources(affected, refreshError);
            });
        error.clear();
        if (!sc13::bootstrap::g_modManager->Initialize(error)) {
            AsyncLogger::Instance().WriteFormat(
                Level::Warning,
                "Mod manager initialized with isolated failures: %s", error.c_str());
        }
        const std::vector<sc13::mods::ModUiEntry> mods =
            sc13::bootstrap::g_modManager->Snapshot();
        for (const sc13::mods::ModUiEntry& mod : mods) {
            AsyncLogger::Instance().WriteEventFormat(
                mod.state == sc13::mods::ModState::Failed ? Level::Warning : Level::Info,
                sc13::logging::SourceType::Mod,
                mod.id.empty() ? mod.name.c_str() : mod.id.c_str(),
                "Mod id=%s name=%s version=%s state=%s diagnostic=%s",
                mod.id.c_str(), mod.name.c_str(), mod.version.c_str(),
                sc13::mods::ModStateName(mod.state), mod.diagnostic.c_str());
        }
        for (const sc13::mods::PatchConflict& conflict :
             sc13::bootstrap::g_modManager->Conflicts()) {
            AsyncLogger::Instance().WriteFormat(
                Level::Warning,
                "Property conflict TGI=%s property=0x%08X earlier=%s later=%s; "
                "activation order applies",
                sc13::core::ToString(conflict.target).c_str(), conflict.propertyId,
                conflict.earlierOwner.c_str(), conflict.laterOwner.c_str());
        }
        sc13::ui::ConfigureSimoderOverlay(*sc13::bootstrap::g_modManager);

#if defined(SC13_ENABLE_DISCOVERY_TRACE)
        const std::filesystem::path captureDirectory = dllPath.parent_path() / L"logs" / L"captures";
        error.clear();
        if (!sc13::runtime::StartImageObserver(
                GetModuleHandleW(nullptr), captureDirectory, error)) {
            AsyncLogger::Instance().WriteFormat(
                Level::Warning, "Runtime image observer was not started: %s", error.c_str());
        } else {
            AsyncLogger::Instance().WriteFormat(
                Level::Info, "Runtime image observer started; captures: %ls",
                captureDirectory.c_str());
        }
#endif

        std::vector<sc13::reverse::ModuleInfo> modules;
        error.clear();
        if (sc13::reverse::EnumerateCurrentModules(modules, error)) {
            AsyncLogger::Instance().WriteFormat(
                Level::Info, "Loaded modules at bootstrap: %zu", modules.size());
            for (const sc13::reverse::ModuleInfo& module : modules) {
                AsyncLogger::Instance().WriteFormat(
                    Level::Trace, "Module base=0x%08llX size=0x%08X path=%ls",
                    static_cast<unsigned long long>(module.base), module.size, module.path.c_str());
            }
        } else {
            AsyncLogger::Instance().WriteFormat(
                Level::Warning, "Module enumeration failed: %s", error.c_str());
        }

        error.clear();
        if (!sc13::hooks::InstallTraceHooks(fingerprint, error)) {
            AsyncLogger::Instance().WriteFormat(
                Level::Error, "Resource instrumentation disabled: %s", error.c_str());
            AsyncLogger::Instance().Write(
                Level::Warning,
                "No hook remains installed; the game continues without SC13 resource patches");
            return 14;
        }
        AsyncLogger::Instance().Write(
            Level::Info,
            "Exact-build PROP deserializer hook installed; declarative active mod patches "
            "are enabled at the verified pre-consumer boundary");
#if defined(SC13_ENABLE_DISCOVERY_TRACE)
        AsyncLogger::Instance().Write(
            Level::Warning,
            "Discovery tracing is enabled for this build; package I/O and auxiliary resource "
            "hooks are diagnostic-only");
#endif
        AsyncLogger::Instance().Write(
            Level::Info,
            "Runtime patches are exact-build/TGI/type gated, rebuild from retained vanilla, "
            "and never write package files");
        return 0;
    } catch (...) {
        AsyncLogger::Instance().Write(
            Level::Error, "Initialization caught an exception; no unsafe game boundary was crossed");
        return 15;
    }
}

extern "C" DWORD WINAPI SC13_Shutdown(void*) noexcept {
    sc13::hooks::UninstallTraceHooks();
#if defined(SC13_ENABLE_DISCOVERY_TRACE)
    sc13::runtime::StopImageObserver();
#endif
    sc13::runtime::StopPatchAppliedSignal();
    sc13::bootstrap::ResetModServices();
    sc13::logging::AsyncLogger::Instance().Write(
        sc13::logging::Level::Info, "SC13 Mod Loader shutting down");
    sc13::logging::AsyncLogger::Instance().Stop();
    sc13::bootstrap::g_initialized.store(false);
    return 0;
}
