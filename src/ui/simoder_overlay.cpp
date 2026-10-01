#include "ui/simoder_overlay.hpp"

#include "logging/async_logger.hpp"
#include "mods/mod_manager.hpp"
#include "runtime/runtime_resource_cache.hpp"
#include "mods/mod_types.hpp"

#include <Windows.h>
#include <d3d9.h>
#include <MinHook.h>

#include <imgui.h>
#include <imgui_impl_dx9.h>
#include <imgui_impl_win32.h>

#include <array>
#include <algorithm>
#include <atomic>
#include <cstdint>
#include <string>
#include <vector>

/** Forwards Win32 messages to the official ImGui backend without changing its header. */
extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(
    HWND window,
    UINT message,
    WPARAM word,
    LPARAM value);

namespace sc13::ui {
namespace {

using EndSceneFunction = HRESULT(WINAPI*)(IDirect3DDevice9*);
using ResetFunction = HRESULT(WINAPI*)(IDirect3DDevice9*, D3DPRESENT_PARAMETERS*);

constexpr unsigned int kResetVtableIndex = 16U;
constexpr unsigned int kEndSceneVtableIndex = 42U;

EndSceneFunction g_originalEndScene = nullptr;
ResetFunction g_originalReset = nullptr;
void* g_endSceneAddress = nullptr;
void* g_resetAddress = nullptr;
mods::ModManager* g_manager = nullptr;
HWND g_gameWindow = nullptr;
WNDPROC g_originalWindowProcedure = nullptr;
std::atomic_bool g_initialized = false;
std::atomic_bool g_initializationAttempted = false;
bool g_panelOpen = false;
std::string g_feedback;

/** Stores the last sampled presentation state so steady frames do not flood the log. */
struct PresentationHealth final {
    HRESULT cooperativeLevel{};
    bool foreground{};
    bool focused{};
    bool iconic{};
    bool visible{};
    std::uint32_t overlayMask{};
    bool initialized{};
};

PresentationHealth g_lastPresentationHealth;
ULONGLONG g_lastHealthSampleTick{};
ULONGLONG g_lastHealthLogTick{};

/** Describes one common third-party overlay module without coupling behavior to its vendor. */
struct OverlayModule final {
    const wchar_t* fileName;
    const char* label;
};

constexpr std::array<OverlayModule, 8U> kOverlayModules{{
    {L"nvspcap.dll", "NVIDIA ShadowPlay"},
    {L"igo32.dll", "EA in-game overlay"},
    {L"DiscordHook.dll", "Discord overlay"},
    {L"GameOverlayRenderer.dll", "Steam overlay"},
    {L"RTSSHooks.dll", "RivaTuner Statistics Server"},
    {L"NahimicOSD.dll", "Nahimic OSD"},
    {L"graphics-hook32.dll", "OBS game capture"},
    {L"owclient.dll", "Overwolf overlay"},
}};

/** Returns the focus window of the foreground GUI thread, if Windows exposes one. */
[[nodiscard]] HWND ForegroundFocusWindow() noexcept {
    const HWND foreground = GetForegroundWindow();
    if (foreground == nullptr) {
        return nullptr;
    }
    const DWORD thread = GetWindowThreadProcessId(foreground, nullptr);
    GUITHREADINFO information{};
    information.cbSize = sizeof(information);
    return thread != 0U && GetGUIThreadInfo(thread, &information) != FALSE
        ? information.hwndFocus
        : nullptr;
}

/** Builds a stable bit mask for known overlay modules currently loaded in the game. */
[[nodiscard]] std::uint32_t LoadedOverlayMask() noexcept {
    std::uint32_t mask = 0U;
    for (std::size_t index = 0U; index < kOverlayModules.size(); ++index) {
        if (GetModuleHandleW(kOverlayModules[index].fileName) != nullptr) {
            mask |= 1U << index;
        }
    }
    return mask;
}

/** Logs newly loaded or unloaded overlay modules represented by a changed mask. */
void LogOverlayMaskChange(std::uint32_t previous, std::uint32_t current) noexcept {
    const std::uint32_t changed = previous ^ current;
    for (std::size_t index = 0U; index < kOverlayModules.size(); ++index) {
        const std::uint32_t bit = 1U << index;
        if ((changed & bit) == 0U) {
            continue;
        }
        logging::AsyncLogger::Instance().WriteFormat(
            logging::Level::Info,
            "Overlay module %s: %s",
            kOverlayModules[index].label,
            (current & bit) != 0U ? "loaded" : "unloaded");
    }
}

/** Samples D3D9 and Win32 health once per second and logs only transitions or a heartbeat. */
void SamplePresentationHealth(IDirect3DDevice9* device) noexcept {
    const ULONGLONG now = GetTickCount64();
    if (device == nullptr || now - g_lastHealthSampleTick < 1'000ULL) {
        return;
    }
    g_lastHealthSampleTick = now;

    PresentationHealth current;
    current.cooperativeLevel = device->TestCooperativeLevel();
    current.foreground = g_gameWindow != nullptr && GetForegroundWindow() == g_gameWindow;
    current.focused = g_gameWindow != nullptr && ForegroundFocusWindow() == g_gameWindow;
    current.iconic = g_gameWindow != nullptr && IsIconic(g_gameWindow) != FALSE;
    current.visible = g_gameWindow != nullptr && IsWindowVisible(g_gameWindow) != FALSE;
    current.overlayMask = LoadedOverlayMask();
    current.initialized = true;

    const bool changed = !g_lastPresentationHealth.initialized ||
        current.cooperativeLevel != g_lastPresentationHealth.cooperativeLevel ||
        current.foreground != g_lastPresentationHealth.foreground ||
        current.focused != g_lastPresentationHealth.focused ||
        current.iconic != g_lastPresentationHealth.iconic ||
        current.visible != g_lastPresentationHealth.visible ||
        current.overlayMask != g_lastPresentationHealth.overlayMask;
    const bool heartbeat = now - g_lastHealthLogTick >= 30'000ULL;
    if (changed || heartbeat) {
        if (g_lastPresentationHealth.initialized &&
            current.overlayMask != g_lastPresentationHealth.overlayMask) {
            LogOverlayMaskChange(g_lastPresentationHealth.overlayMask, current.overlayMask);
        } else if (!g_lastPresentationHealth.initialized) {
            LogOverlayMaskChange(0U, current.overlayMask);
        }
        logging::AsyncLogger::Instance().WriteFormat(
            logging::Level::Debug,
            "Presentation health cooperative=0x%08lX foreground=%u focus=%u iconic=%u "
            "visible=%u overlayMask=0x%08X",
            static_cast<unsigned long>(current.cooperativeLevel),
            current.foreground ? 1U : 0U,
            current.focused ? 1U : 0U,
            current.iconic ? 1U : 0U,
            current.visible ? 1U : 0U,
            current.overlayMask);
        g_lastHealthLogTick = now;
    }
    g_lastPresentationHealth = current;
}

/** Returns a concise label for window events relevant to overlay focus transitions. */
[[nodiscard]] const char* WindowEventName(UINT message) noexcept {
    switch (message) {
        case WM_ACTIVATE: return "WM_ACTIVATE";
        case WM_ACTIVATEAPP: return "WM_ACTIVATEAPP";
        case WM_SETFOCUS: return "WM_SETFOCUS";
        case WM_KILLFOCUS: return "WM_KILLFOCUS";
        case WM_SIZE: return "WM_SIZE";
        case WM_ENTERSIZEMOVE: return "WM_ENTERSIZEMOVE";
        case WM_EXITSIZEMOVE: return "WM_EXITSIZEMOVE";
        case WM_DISPLAYCHANGE: return "WM_DISPLAYCHANGE";
        default: return nullptr;
    }
}

/** Logs one relevant window transition after preserving the game's original result. */
void LogWindowTransition(
    UINT message,
    WPARAM word,
    LPARAM value,
    LRESULT result,
    bool imguiHandled) noexcept {
    const char* const name = WindowEventName(message);
    if (name == nullptr) {
        return;
    }
    logging::AsyncLogger::Instance().WriteFormat(
        logging::Level::Debug,
        "Window event %s wParam=0x%llX lParam=0x%llX result=0x%llX imguiHandled=%u "
        "foreground=%u focus=%u",
        name,
        static_cast<unsigned long long>(word),
        static_cast<unsigned long long>(value),
        static_cast<unsigned long long>(result),
        imguiHandled ? 1U : 0U,
        g_gameWindow != nullptr && GetForegroundWindow() == g_gameWindow ? 1U : 0U,
        g_gameWindow != nullptr && ForegroundFocusWindow() == g_gameWindow ? 1U : 0U);
}

/** Provides the temporary hidden window used only to resolve D3D9 method addresses. */
LRESULT CALLBACK DummyWindowProcedure(
    HWND window,
    UINT message,
    WPARAM word,
    LPARAM value) noexcept {
    return DefWindowProcW(window, message, word, value);
}

/** Owns temporary D3D9 discovery resources and releases them on every exit path. */
class D3dDiscoveryContext final {
public:
    /** Releases the dummy device, API object, window, and registered class. */
    ~D3dDiscoveryContext() {
        if (device != nullptr) {
            device->Release();
        }
        if (api != nullptr) {
            api->Release();
        }
        if (window != nullptr) {
            DestroyWindow(window);
        }
        if (classAtom != 0U) {
            UnregisterClassW(className, instance);
        }
    }

    D3dDiscoveryContext() = default;
    D3dDiscoveryContext(const D3dDiscoveryContext&) = delete;
    D3dDiscoveryContext& operator=(const D3dDiscoveryContext&) = delete;

    HINSTANCE instance{GetModuleHandleW(nullptr)};
    const wchar_t* className{L"SimoderD3D9DiscoveryWindow"};
    ATOM classAtom{};
    HWND window{};
    IDirect3D9* api{};
    IDirect3DDevice9* device{};
};

/** Resolves the shared D3D9 Reset and EndScene implementation addresses. */
[[nodiscard]] bool ResolveD3dMethods(
    void*& reset,
    void*& endScene,
    std::string& error) noexcept {
    D3dDiscoveryContext context;
    WNDCLASSEXW windowClass{};
    windowClass.cbSize = sizeof(windowClass);
    windowClass.lpfnWndProc = &DummyWindowProcedure;
    windowClass.hInstance = context.instance;
    windowClass.lpszClassName = context.className;
    context.classAtom = RegisterClassExW(&windowClass);
    if (context.classAtom == 0U) {
        error = "Could not register the D3D9 discovery window class";
        return false;
    }
    context.window = CreateWindowExW(
        0U, context.className, L"", WS_OVERLAPPEDWINDOW,
        0, 0, 64, 64, nullptr, nullptr, context.instance, nullptr);
    if (context.window == nullptr) {
        error = "Could not create the D3D9 discovery window";
        return false;
    }
    context.api = Direct3DCreate9(D3D_SDK_VERSION);
    if (context.api == nullptr) {
        error = "Direct3DCreate9 failed for overlay discovery";
        return false;
    }
    D3DPRESENT_PARAMETERS presentation{};
    presentation.Windowed = TRUE;
    presentation.SwapEffect = D3DSWAPEFFECT_DISCARD;
    presentation.hDeviceWindow = context.window;
    presentation.BackBufferFormat = D3DFMT_UNKNOWN;
    HRESULT result = context.api->CreateDevice(
        D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, context.window,
        D3DCREATE_SOFTWARE_VERTEXPROCESSING, &presentation, &context.device);
    if (FAILED(result)) {
        result = context.api->CreateDevice(
            D3DADAPTER_DEFAULT, D3DDEVTYPE_REF, context.window,
            D3DCREATE_SOFTWARE_VERTEXPROCESSING, &presentation, &context.device);
    }
    if (FAILED(result) || context.device == nullptr) {
        error = "Could not create a temporary D3D9 device for overlay discovery";
        return false;
    }
    void** const vtable = *reinterpret_cast<void***>(context.device);
    reset = vtable[kResetVtableIndex];
    endScene = vtable[kEndSceneVtableIndex];
    if (reset == nullptr || endScene == nullptr || reset == endScene) {
        error = "D3D9 device vtable did not expose unique Reset and EndScene methods";
        return false;
    }
    return true;
}

/** Routes captured input to ImGui while preserving untouched game window behavior. */
LRESULT CALLBACK HookWindowProcedure(
    HWND window,
    UINT message,
    WPARAM word,
    LPARAM value) noexcept {
    const bool imguiHandled = g_initialized.load(std::memory_order_acquire) &&
        ImGui_ImplWin32_WndProcHandler(window, message, word, value) != 0;
    const LRESULT result = imguiHandled
        ? 1
        : (g_originalWindowProcedure != nullptr
               ? CallWindowProcW(g_originalWindowProcedure, window, message, word, value)
               : DefWindowProcW(window, message, word, value));
    LogWindowTransition(message, word, value, result, imguiHandled);
    return result;
}

/** Initializes one ImGui context against the actual game device and focus window. */
[[nodiscard]] bool InitializeImGui(IDirect3DDevice9* device) noexcept {
    if (device == nullptr || g_manager == nullptr) {
        return false;
    }
    D3DDEVICE_CREATION_PARAMETERS creation{};
    if (FAILED(device->GetCreationParameters(&creation)) ||
        creation.hFocusWindow == nullptr) {
        return false;
    }
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGui::StyleColorsDark();
    if (!ImGui_ImplWin32_Init(creation.hFocusWindow) ||
        !ImGui_ImplDX9_Init(device)) {
        ImGui_ImplDX9_Shutdown();
        ImGui_ImplWin32_Shutdown();
        ImGui::DestroyContext();
        return false;
    }
    SetLastError(ERROR_SUCCESS);
    const LONG_PTR previous = SetWindowLongPtrW(
        creation.hFocusWindow, GWLP_WNDPROC,
        reinterpret_cast<LONG_PTR>(&HookWindowProcedure));
    if (previous == 0 && GetLastError() != ERROR_SUCCESS) {
        ImGui_ImplDX9_Shutdown();
        ImGui_ImplWin32_Shutdown();
        ImGui::DestroyContext();
        return false;
    }
    g_gameWindow = creation.hFocusWindow;
    g_originalWindowProcedure = reinterpret_cast<WNDPROC>(previous);
    g_initialized.store(true, std::memory_order_release);
    logging::AsyncLogger::Instance().Write(
        logging::Level::Info, "Simoder ImGui overlay initialized");
    return true;
}

/** Renders the persistent launcher button and the user-facing mod table. */
void RenderSimoder() {
    ImGui_ImplDX9_NewFrame();
    ImGui_ImplWin32_NewFrame();
    ImGui::NewFrame();

    ImGui::SetNextWindowBgAlpha(0.88F);
    ImGui::Begin(
        "##SimoderLauncher", nullptr,
        ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
            ImGuiWindowFlags_NoSavedSettings);
    if (ImGui::Button("Simoder")) {
        g_panelOpen = !g_panelOpen;
    }
    ImGui::End();

    if (g_panelOpen) {
        ImGui::SetNextWindowSize(ImVec2(720.0F, 420.0F), ImGuiCond_FirstUseEver);
        if (ImGui::Begin("Simoder", &g_panelOpen)) {
            if (ImGui::Button("Refresh") && g_manager != nullptr) {
                std::string error;
                const bool refreshed = g_manager->Refresh(error);
                g_feedback = refreshed
                    ? "Mod list refreshed. Existing live objects may require recreation or map reload."
                    : "Refresh failed: " + error;
                logging::AsyncLogger::Instance().WriteFormat(
                    refreshed ? logging::Level::Info : logging::Level::Error,
                    "Simoder Refresh: %s", g_feedback.c_str());
            }
            if (!g_feedback.empty()) {
                ImGui::SameLine();
                ImGui::TextWrapped("%s", g_feedback.c_str());
            }
            ImGui::Separator();

            const std::vector<mods::ModUiEntry> entries =
                g_manager == nullptr ? std::vector<mods::ModUiEntry>{}
                                     : g_manager->Snapshot();
            const auto runtimeObservations = g_manager == nullptr ? std::vector<runtime::RuntimeResourceObservation>{}
                : g_manager->RuntimeObservations();
            if (ImGui::BeginTable(
                    "ModTable", 5,
                    ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
                        ImGuiTableFlags_Resizable | ImGuiTableFlags_SizingStretchProp)) {
                ImGui::TableSetupColumn("Name");
                ImGui::TableSetupColumn("Version");
                ImGui::TableSetupColumn("Author");
                ImGui::TableSetupColumn("State");
                ImGui::TableSetupColumn("Enabled", ImGuiTableColumnFlags_WidthFixed, 80.0F);
                ImGui::TableHeadersRow();
                for (const mods::ModUiEntry& entry : entries) {
                    ImGui::PushID(entry.id.empty() ? entry.name.c_str() : entry.id.c_str());
                    ImGui::TableNextRow();
                    ImGui::TableSetColumnIndex(0);
                    ImGui::TextUnformatted(entry.name.c_str());
                    if (!entry.diagnostic.empty() && ImGui::IsItemHovered()) {
                        ImGui::SetTooltip("%s", entry.diagnostic.c_str());
                    }
                    if (ImGui::TreeNode("Resolved overrides")) {
                        for (const auto& patch : entry.patches) {
                            ImGui::TextWrapped("%s TGI=%s", patch.name.c_str(),
                                core::ToString(patch.target).c_str());
                            if (!patch.source.empty()) ImGui::TextWrapped("Source: %s", patch.source.c_str());
                            const auto observedCount = std::count_if(runtimeObservations.begin(), runtimeObservations.end(),
                                [&patch](const auto& observation) { return observation.key.target == patch.target; });
                            const auto appliedCount = std::count_if(runtimeObservations.begin(), runtimeObservations.end(),
                                [&patch](const auto& observation) { return observation.key.target == patch.target && observation.status == "applied"; });
                            ImGui::Text("Retained observations: %llu; applied: %llu (historical)",
                                static_cast<unsigned long long>(observedCount), static_cast<unsigned long long>(appliedCount));
                            for (const auto& property : patch.properties) {
                                ImGui::TextWrapped("%s [0x%08X] %s %.9g", property.name.c_str(),
                                    property.propertyId, mods::PatchOperationName(property.operation), property.value);
                                if (!property.source.empty()) ImGui::TextWrapped("Source: %s", property.source.c_str());
                                if (!property.description.empty()) ImGui::TextWrapped("%s", property.description.c_str());
                                if (!property.unit.empty()) ImGui::TextWrapped("Unit: %s", property.unit.c_str());
                                if (!property.origin.empty()) ImGui::TextWrapped("Origin: %s", property.origin.c_str());
                                for (const auto& observation : runtimeObservations) {
                                    if (observation.key.target != patch.target) continue;
                                    const auto vanilla = core::ReadRuntimeFloat(observation.vanilla, property.propertyId);
                                    const auto applied = core::ReadRuntimeFloat(observation.applied, property.propertyId);
                                    ImGui::TextWrapped("Observed instance=%llu generation=%llu %s: %s",
                                        static_cast<unsigned long long>(observation.key.resourceAddress),
                                        static_cast<unsigned long long>(observation.generation),
                                        observation.status.c_str(), observation.diagnostic.c_str());
                                    if (vanilla && applied) ImGui::Text("Historical vanilla %.9g -> committed %.9g", *vanilla, *applied);
                                }
                            }
                            if (std::none_of(runtimeObservations.begin(), runtimeObservations.end(),
                                [&patch](const auto& observation) { return observation.key.target == patch.target; }))
                                ImGui::TextUnformatted("No retained observation for this resource.");
                        }
                        ImGui::TreePop();
                    }
                    if (entry.canReload && ImGui::TreeNode("Pending changes (current -> pending)")) {
                        ImGui::TextUnformatted("Current operations:");
                        for (const auto& patch : entry.patches) for (const auto& property : patch.properties)
                            ImGui::TextWrapped("%s %s [0x%08X] %s %.9g", core::ToString(patch.target).c_str(),
                                property.name.c_str(), property.propertyId, mods::PatchOperationName(property.operation), property.value);
                        ImGui::TextUnformatted("Pending operations:");
                        for (const auto& patch : entry.pendingPatches) for (const auto& property : patch.properties)
                            ImGui::TextWrapped("%s %s [0x%08X] %s %.9g", core::ToString(patch.target).c_str(),
                                property.name.c_str(), property.propertyId, mods::PatchOperationName(property.operation), property.value);
                        if (ImGui::Button("Apply validated reload")) {
                            std::string error;
                            g_feedback = g_manager->Reload(entry.id, error, entry.pendingRevision) ? "Reload published; inspect resource outcomes." : "Reload rejected: " + error;
                        }
                        ImGui::TreePop();
                    }
                    ImGui::TableSetColumnIndex(1);
                    ImGui::TextUnformatted(entry.version.c_str());
                    ImGui::TableSetColumnIndex(2);
                    ImGui::TextUnformatted(entry.author.c_str());
                    ImGui::TableSetColumnIndex(3);
                    ImGui::TextUnformatted(mods::ModStateName(entry.state));
                    ImGui::TableSetColumnIndex(4);
                    bool enabled = entry.canDisable;
                    const bool interactive = entry.canEnable || entry.canDisable;
                    ImGui::BeginDisabled(!interactive);
                    if (ImGui::Checkbox("##Enabled", &enabled) && g_manager != nullptr) {
                        std::string error;
                        if (!g_manager->SetEnabled(entry.id, enabled, error)) {
                            g_feedback = "State change failed for " + entry.name + ": " + error;
                        } else {
                            g_feedback = entry.name +
                                (enabled ? " enabled" : " disabled") +
                                ". Live resources and future loads use the new state.";
                            for (const mods::PatchConflict& conflict :
                                 g_manager->Conflicts()) {
                                logging::AsyncLogger::Instance().WriteFormat(
                                    logging::Level::Warning,
                                    "Property conflict TGI=%s property=0x%08X earlier=%s "
                                    "later=%s; activation order applies",
                                    core::ToString(conflict.target).c_str(),
                                    conflict.propertyId,
                                    conflict.earlierOwner.c_str(),
                                    conflict.laterOwner.c_str());
                            }
                        }
                    }
                    ImGui::EndDisabled();
                    ImGui::PopID();
                }
                ImGui::EndTable();
            }
        }
        ImGui::End();
    }

    ImGui::EndFrame();
    ImGui::Render();
    ImGui_ImplDX9_RenderDrawData(ImGui::GetDrawData());
}

/** Renders one overlay frame and always continues into the original EndScene. */
HRESULT WINAPI HookEndScene(IDirect3DDevice9* device) noexcept {
    try {
        if (!g_initializationAttempted.exchange(true, std::memory_order_acq_rel)) {
            if (!InitializeImGui(device)) {
                logging::AsyncLogger::Instance().Write(
                    logging::Level::Error,
                    "Simoder overlay initialization failed; runtime mods continue without UI");
            }
        }
        if (g_initialized.load(std::memory_order_acquire)) {
            SamplePresentationHealth(device);
            RenderSimoder();
        }
    } catch (...) {
        logging::AsyncLogger::Instance().Write(
            logging::Level::Error,
            "Simoder overlay frame failed; original EndScene continues");
    }
    return g_originalEndScene(device);
}

/** Invalidates ImGui device objects around the game's original D3D9 Reset. */
HRESULT WINAPI HookReset(
    IDirect3DDevice9* device,
    D3DPRESENT_PARAMETERS* presentation) noexcept {
    const bool initialized = g_initialized.load(std::memory_order_acquire);
    if (initialized) {
        ImGui_ImplDX9_InvalidateDeviceObjects();
    }
    logging::AsyncLogger::Instance().WriteFormat(
        logging::Level::Debug,
        "D3D9 Reset begin device=%p window=%p windowed=%u backBuffer=%ux%u format=%u",
        device,
        presentation != nullptr ? presentation->hDeviceWindow : nullptr,
        presentation != nullptr && presentation->Windowed != FALSE ? 1U : 0U,
        presentation != nullptr ? presentation->BackBufferWidth : 0U,
        presentation != nullptr ? presentation->BackBufferHeight : 0U,
        presentation != nullptr ? static_cast<unsigned int>(presentation->BackBufferFormat) : 0U);
    const HRESULT result = g_originalReset(device, presentation);
    if (SUCCEEDED(result) && initialized) {
        ImGui_ImplDX9_CreateDeviceObjects();
    }
    logging::AsyncLogger::Instance().WriteFormat(
        SUCCEEDED(result) ? logging::Level::Debug : logging::Level::Warning,
        "D3D9 Reset end result=0x%08lX cooperative=0x%08lX",
        static_cast<unsigned long>(result),
        device != nullptr
            ? static_cast<unsigned long>(device->TestCooperativeLevel())
            : static_cast<unsigned long>(E_POINTER));
    return result;
}

}  // namespace

void ConfigureSimoderOverlay(mods::ModManager& manager) noexcept {
    g_manager = &manager;
}

bool CreateSimoderOverlayHooks(std::string& error) noexcept {
    if (g_manager == nullptr) {
        error = "Simoder overlay view model was not configured";
        return false;
    }
    if (!ResolveD3dMethods(g_resetAddress, g_endSceneAddress, error)) {
        return false;
    }
    const MH_STATUS resetStatus = MH_CreateHook(
        g_resetAddress, reinterpret_cast<LPVOID>(&HookReset),
        reinterpret_cast<LPVOID*>(&g_originalReset));
    if (resetStatus != MH_OK) {
        error = std::string("MH_CreateHook failed for D3D9 Reset: ") +
                MH_StatusToString(resetStatus);
        return false;
    }
    const MH_STATUS endSceneStatus = MH_CreateHook(
        g_endSceneAddress, reinterpret_cast<LPVOID>(&HookEndScene),
        reinterpret_cast<LPVOID*>(&g_originalEndScene));
    if (endSceneStatus != MH_OK) {
        MH_RemoveHook(g_resetAddress);
        g_originalReset = nullptr;
        error = std::string("MH_CreateHook failed for D3D9 EndScene: ") +
                MH_StatusToString(endSceneStatus);
        return false;
    }
    return true;
}

void ShutdownSimoderOverlay() noexcept {
    try {
        if (g_gameWindow != nullptr && g_originalWindowProcedure != nullptr &&
            IsWindow(g_gameWindow) != FALSE) {
            SetWindowLongPtrW(
                g_gameWindow, GWLP_WNDPROC,
                reinterpret_cast<LONG_PTR>(g_originalWindowProcedure));
        }
        if (g_initialized.exchange(false, std::memory_order_acq_rel)) {
            ImGui_ImplDX9_Shutdown();
            ImGui_ImplWin32_Shutdown();
            ImGui::DestroyContext();
        }
    } catch (...) {
        logging::AsyncLogger::Instance().Write(
            logging::Level::Error,
            "Simoder overlay shutdown caught an exception");
    }
    g_gameWindow = nullptr;
    g_originalWindowProcedure = nullptr;
    g_originalEndScene = nullptr;
    g_originalReset = nullptr;
    g_endSceneAddress = nullptr;
    g_resetAddress = nullptr;
    g_manager = nullptr;
    g_panelOpen = false;
    g_feedback.clear();
    g_initializationAttempted.store(false, std::memory_order_release);
    g_lastPresentationHealth = {};
    g_lastHealthSampleTick = 0U;
    g_lastHealthLogTick = 0U;
}

}  // namespace sc13::ui
