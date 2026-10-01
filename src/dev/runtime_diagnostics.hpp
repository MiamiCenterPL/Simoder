#pragma once
#include "mods/mod_manager.hpp"
#include <condition_variable>
#include <thread>

namespace sc13::dev {
/** @summary Serializes bounded cached runtime evidence without implying gameplay acceptance. */
[[nodiscard]] std::string RuntimeDiagnosticsJson(const mods::ModManager& manager, std::uint32_t processId);
/** @summary Watches definitions and atomically publishes read-only developer snapshots. */
class RuntimeDiagnostics final {
public:
    /** @summary Starts an owned worker after the runtime hook is installed. */
    RuntimeDiagnostics(mods::ModManager& manager, std::filesystem::path destination);
    /** @summary Stops and joins the worker before manager or hook destruction. */
    ~RuntimeDiagnostics();
    RuntimeDiagnostics(const RuntimeDiagnostics&) = delete;
    RuntimeDiagnostics& operator=(const RuntimeDiagnostics&) = delete;
private:
    std::mutex waitMutex_;
    std::condition_variable wake_;
    std::jthread worker_;
};
}
