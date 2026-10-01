#include <Windows.h>
#include <TlHelp32.h>

#include "runtime/patch_signal.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace {

/** Owns a Windows handle and closes it outside game loader-lock contexts. */
class UniqueHandle final {
public:
    explicit UniqueHandle(HANDLE value = nullptr) noexcept : value_(value) {}

    /** Closes an owned handle. */
    ~UniqueHandle() {
        if (value_ != nullptr && value_ != INVALID_HANDLE_VALUE) {
            CloseHandle(value_);
        }
    }

    UniqueHandle(const UniqueHandle&) = delete;
    UniqueHandle& operator=(const UniqueHandle&) = delete;

    /** Moves ownership from another wrapper. */
    UniqueHandle(UniqueHandle&& other) noexcept : value_(other.value_) { other.value_ = nullptr; }

    /** Returns the raw Windows handle. */
    [[nodiscard]] HANDLE get() const noexcept { return value_; }

    /** Returns true for a usable Windows handle. */
    [[nodiscard]] explicit operator bool() const noexcept {
        return value_ != nullptr && value_ != INVALID_HANDLE_VALUE;
    }

private:
    HANDLE value_{};
};

/** Prints launcher syntax for attach and early suspended-launch modes. */
void PrintUsage() {
    std::wcerr
        << L"Usage:\n"
        << L"  simoder --self-check [dll-path]\n"
        << L"  simoder --attach [pid] [dll-path]\n"
        << L"  simoder --watch-attach <excluded-pid> [dll-path]\n"
        << L"  simoder --detach <pid> [dll-path]\n"
        << L"  simoder --launch <SimCity.exe> [dll-path]\n";
}

/** Resolves the default DLL next to the launcher executable. */
[[nodiscard]] std::optional<std::filesystem::path> DefaultDllPath() {
    std::vector<wchar_t> buffer(32768);
    const DWORD length = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
    if (length == 0 || length == buffer.size()) {
        return std::nullopt;
    }
    return std::filesystem::path(buffer.data(), buffer.data() + length).parent_path() /
           L"sc13modloader.dll";
}

/** Parses a decimal process identifier without accepting trailing text. */
[[nodiscard]] std::optional<DWORD> ParseProcessId(std::wstring_view text) noexcept {
    std::string narrow;
    narrow.reserve(text.size());
    for (const wchar_t character : text) {
        if (character < L'0' || character > L'9') {
            return std::nullopt;
        }
        narrow.push_back(static_cast<char>(character));
    }
    unsigned long value = 0;
    const auto parsed = std::from_chars(narrow.data(), narrow.data() + narrow.size(), value, 10);
    if (narrow.empty() || parsed.ec != std::errc{} || parsed.ptr != narrow.data() + narrow.size() ||
        value == 0 || value > MAXDWORD) {
        return std::nullopt;
    }
    return static_cast<DWORD>(value);
}

/** Finds every process whose executable name is exactly SimCity.exe. */
[[nodiscard]] std::vector<DWORD> FindSimCityProcesses() {
    std::vector<DWORD> results;
    UniqueHandle snapshot(CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0));
    if (!snapshot) {
        return results;
    }
    PROCESSENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    if (Process32FirstW(snapshot.get(), &entry) == FALSE) {
        return results;
    }
    do {
        if (_wcsicmp(entry.szExeFile, L"SimCity.exe") == 0) {
            results.push_back(entry.th32ProcessID);
        }
    } while (Process32NextW(snapshot.get(), &entry) != FALSE);
    return results;
}

/** Finds one module base in a remote process using its case-insensitive base name. */
[[nodiscard]] std::optional<std::uintptr_t> FindRemoteModule(
    DWORD processId, std::wstring_view moduleName) {
    for (int attempt = 0; attempt < 8; ++attempt) {
        UniqueHandle snapshot(CreateToolhelp32Snapshot(
            TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, processId));
        if (!snapshot) {
            if (GetLastError() == ERROR_BAD_LENGTH) {
                continue;
            }
            return std::nullopt;
        }
        MODULEENTRY32W entry{};
        entry.dwSize = sizeof(entry);
        if (Module32FirstW(snapshot.get(), &entry) == FALSE) {
            continue;
        }
        do {
            if (_wcsicmp(entry.szModule, std::wstring(moduleName).c_str()) == 0) {
                return reinterpret_cast<std::uintptr_t>(entry.modBaseAddr);
            }
        } while (Module32NextW(snapshot.get(), &entry) != FALSE);
    }
    return std::nullopt;
}

/** Resolves a local export's actual owner module and maps the same RVA in a remote process. */
[[nodiscard]] std::optional<std::uintptr_t> ResolveRemoteProcedure(
    DWORD processId, const wchar_t* requestedModule, const char* procedureName) {
    const HMODULE requested = GetModuleHandleW(requestedModule);
    if (requested == nullptr) {
        return std::nullopt;
    }
    const FARPROC procedure = GetProcAddress(requested, procedureName);
    if (procedure == nullptr) {
        return std::nullopt;
    }

    HMODULE owner = nullptr;
    if (GetModuleHandleExW(
            GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            reinterpret_cast<LPCWSTR>(procedure), &owner) == FALSE || owner == nullptr) {
        return std::nullopt;
    }
    std::array<wchar_t, 32768> ownerPath{};
    const DWORD length = GetModuleFileNameW(
        owner, ownerPath.data(), static_cast<DWORD>(ownerPath.size()));
    if (length == 0 || length == ownerPath.size()) {
        return std::nullopt;
    }

    const auto remoteOwner = FindRemoteModule(
        processId, std::filesystem::path(ownerPath.data(), ownerPath.data() + length)
                       .filename()
                       .wstring());
    if (!remoteOwner.has_value()) {
        return std::nullopt;
    }
    const std::uintptr_t procedureRva =
        reinterpret_cast<std::uintptr_t>(procedure) - reinterpret_cast<std::uintptr_t>(owner);
    return remoteOwner.value() + procedureRva;
}

/** Closes file handles supplied with debugger events after their metadata is consumed. */
void CloseDebugEventFile(const DEBUG_EVENT& event) noexcept {
    if (event.dwDebugEventCode == CREATE_PROCESS_DEBUG_EVENT &&
        event.u.CreateProcessInfo.hFile != nullptr) {
        CloseHandle(event.u.CreateProcessInfo.hFile);
    } else if (event.dwDebugEventCode == LOAD_DLL_DEBUG_EVENT &&
               event.u.LoadDll.hFile != nullptr) {
        CloseHandle(event.u.LoadDll.hFile);
    }
}

/** Reads the executable entry-point address from a mapped remote PE32 image. */
[[nodiscard]] bool ReadRemoteEntryPoint(
    HANDLE process, const void* imageBase, void*& entryPoint, std::wstring& error) {
    IMAGE_DOS_HEADER dosHeader{};
    SIZE_T bytesRead = 0;
    if (imageBase == nullptr ||
        ReadProcessMemory(
            process, imageBase, &dosHeader, sizeof(dosHeader), &bytesRead) == FALSE ||
        bytesRead != sizeof(dosHeader) || dosHeader.e_magic != IMAGE_DOS_SIGNATURE ||
        dosHeader.e_lfanew <= 0) {
        error = L"Could not read the remote DOS header";
        return false;
    }

    IMAGE_NT_HEADERS32 ntHeaders{};
    const auto* const ntAddress = static_cast<const std::byte*>(imageBase) + dosHeader.e_lfanew;
    if (ReadProcessMemory(
            process, ntAddress, &ntHeaders, sizeof(ntHeaders), &bytesRead) == FALSE ||
        bytesRead != sizeof(ntHeaders) || ntHeaders.Signature != IMAGE_NT_SIGNATURE ||
        ntHeaders.OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR32_MAGIC) {
        error = L"Could not read the remote PE32 headers";
        return false;
    }
    entryPoint = const_cast<std::byte*>(
        static_cast<const std::byte*>(imageBase) + ntHeaders.OptionalHeader.AddressOfEntryPoint);
    return true;
}

/** Replaces one remote instruction byte while preserving its original page protection. */
[[nodiscard]] bool WriteRemoteCodeByte(
    HANDLE process, void* address, std::byte value, std::wstring& error) {
    DWORD originalProtection = 0;
    if (VirtualProtectEx(
            process, address, sizeof(value), PAGE_EXECUTE_READWRITE, &originalProtection) == FALSE) {
        error = L"VirtualProtectEx failed with error " + std::to_wstring(GetLastError());
        return false;
    }
    SIZE_T bytesWritten = 0;
    const BOOL writeResult =
        WriteProcessMemory(process, address, &value, sizeof(value), &bytesWritten);
    DWORD ignoredProtection = 0;
    const BOOL restoreResult = VirtualProtectEx(
        process, address, sizeof(value), originalProtection, &ignoredProtection);
    FlushInstructionCache(process, address, sizeof(value));
    if (writeResult == FALSE || bytesWritten != sizeof(value) || restoreResult == FALSE) {
        error = L"Could not write or protect the temporary entry-point breakpoint";
        return false;
    }
    return true;
}

/** Advances a debug-launched child to its executable entry and leaves the primary thread suspended. */
[[nodiscard]] bool AdvanceToExecutableEntry(
    const PROCESS_INFORMATION& processInfo, std::wstring& error) {
    if (ResumeThread(processInfo.hThread) == static_cast<DWORD>(-1)) {
        error = L"Could not resume the child for loader initialization";
        DebugActiveProcessStop(processInfo.dwProcessId);
        return false;
    }

    constexpr ULONGLONG timeoutMilliseconds = 30'000;
    const ULONGLONG deadline = GetTickCount64() + timeoutMilliseconds;
    void* imageBase = nullptr;
    void* entryPoint = nullptr;
    std::byte originalEntryByte{};
    bool entryBreakpointArmed = false;
    while (GetTickCount64() < deadline) {
        DEBUG_EVENT event{};
        if (WaitForDebugEventEx(&event, 1'000) == FALSE) {
            if (GetLastError() == ERROR_SEM_TIMEOUT) {
                continue;
            }
            error = L"WaitForDebugEventEx failed with error " + std::to_wstring(GetLastError());
            DebugActiveProcessStop(processInfo.dwProcessId);
            return false;
        }

        if (event.dwDebugEventCode == CREATE_PROCESS_DEBUG_EVENT) {
            imageBase = event.u.CreateProcessInfo.lpBaseOfImage;
        }
        CloseDebugEventFile(event);
        const bool initialBreakpoint =
            event.dwDebugEventCode == EXCEPTION_DEBUG_EVENT &&
            event.u.Exception.ExceptionRecord.ExceptionCode == EXCEPTION_BREAKPOINT;
        if (initialBreakpoint && !entryBreakpointArmed) {
            if (!ReadRemoteEntryPoint(processInfo.hProcess, imageBase, entryPoint, error)) {
                ContinueDebugEvent(event.dwProcessId, event.dwThreadId, DBG_CONTINUE);
                DebugActiveProcessStop(processInfo.dwProcessId);
                return false;
            }
            SIZE_T bytesRead = 0;
            if (ReadProcessMemory(
                    processInfo.hProcess, entryPoint, &originalEntryByte,
                    sizeof(originalEntryByte), &bytesRead) == FALSE ||
                bytesRead != sizeof(originalEntryByte)) {
                ContinueDebugEvent(event.dwProcessId, event.dwThreadId, DBG_CONTINUE);
                error = L"Could not read the executable entry-point byte";
                DebugActiveProcessStop(processInfo.dwProcessId);
                return false;
            }
            if (!WriteRemoteCodeByte(
                    processInfo.hProcess, entryPoint, std::byte{0xCC}, error)) {
                ContinueDebugEvent(event.dwProcessId, event.dwThreadId, DBG_CONTINUE);
                DebugActiveProcessStop(processInfo.dwProcessId);
                return false;
            }
            entryBreakpointArmed = true;
            if (ContinueDebugEvent(event.dwProcessId, event.dwThreadId, DBG_CONTINUE) == FALSE) {
                error = L"Could not continue from the system loader breakpoint";
                DebugActiveProcessStop(processInfo.dwProcessId);
                return false;
            }
            continue;
        }

        const bool executableEntryBreakpoint =
            entryBreakpointArmed && initialBreakpoint &&
            event.dwThreadId == processInfo.dwThreadId &&
            event.u.Exception.ExceptionRecord.ExceptionAddress == entryPoint;
        if (executableEntryBreakpoint) {
            if (!WriteRemoteCodeByte(
                    processInfo.hProcess, entryPoint, originalEntryByte, error)) {
                ContinueDebugEvent(event.dwProcessId, event.dwThreadId, DBG_CONTINUE);
                DebugActiveProcessStop(processInfo.dwProcessId);
                return false;
            }

            CONTEXT context{};
            context.ContextFlags = CONTEXT_CONTROL;
            if (GetThreadContext(processInfo.hThread, &context) == FALSE) {
                ContinueDebugEvent(event.dwProcessId, event.dwThreadId, DBG_CONTINUE);
                error = L"Could not read the primary thread context at executable entry";
                DebugActiveProcessStop(processInfo.dwProcessId);
                return false;
            }
            context.Eip = static_cast<DWORD>(reinterpret_cast<std::uintptr_t>(entryPoint));
            if (SetThreadContext(processInfo.hThread, &context) == FALSE) {
                ContinueDebugEvent(event.dwProcessId, event.dwThreadId, DBG_CONTINUE);
                error = L"Could not restore the primary thread instruction pointer";
                DebugActiveProcessStop(processInfo.dwProcessId);
                return false;
            }
            if (SuspendThread(processInfo.hThread) == static_cast<DWORD>(-1)) {
                ContinueDebugEvent(event.dwProcessId, event.dwThreadId, DBG_CONTINUE);
                error = L"Could not retain the primary thread at executable entry";
                DebugActiveProcessStop(processInfo.dwProcessId);
                return false;
            }
            if (ContinueDebugEvent(event.dwProcessId, event.dwThreadId, DBG_CONTINUE) == FALSE) {
                error = L"Could not continue the executable entry breakpoint event";
                DebugActiveProcessStop(processInfo.dwProcessId);
                return false;
            }
            if (DebugActiveProcessStop(processInfo.dwProcessId) == FALSE) {
                error = L"Could not detach the bootstrap debugger";
                return false;
            }
            return true;
        }

        if (event.dwDebugEventCode == EXIT_PROCESS_DEBUG_EVENT) {
            ContinueDebugEvent(event.dwProcessId, event.dwThreadId, DBG_CONTINUE);
            error = L"Child exited before reaching the loader breakpoint";
            return false;
        }

        const DWORD continueStatus = event.dwDebugEventCode == EXCEPTION_DEBUG_EVENT
                                         ? DBG_EXCEPTION_NOT_HANDLED
                                         : DBG_CONTINUE;
        if (ContinueDebugEvent(event.dwProcessId, event.dwThreadId, continueStatus) == FALSE) {
            error = L"ContinueDebugEvent failed with error " + std::to_wstring(GetLastError());
            DebugActiveProcessStop(processInfo.dwProcessId);
            return false;
        }
    }

    error = L"Timed out waiting for the executable entry breakpoint";
    DebugActiveProcessStop(processInfo.dwProcessId);
    return false;
}

/** Resolves a local export RVA without running the target DLL's DllMain. */
[[nodiscard]] std::optional<std::uintptr_t> ExportRva(
    const std::filesystem::path& dllPath, const char* exportName) {
    const HMODULE module = LoadLibraryExW(dllPath.c_str(), nullptr, DONT_RESOLVE_DLL_REFERENCES);
    if (module == nullptr) {
        return std::nullopt;
    }
    const FARPROC procedure = GetProcAddress(module, exportName);
    const std::optional<std::uintptr_t> result = procedure == nullptr ? std::nullopt :
        std::optional<std::uintptr_t>(
            reinterpret_cast<std::uintptr_t>(procedure) - reinterpret_cast<std::uintptr_t>(module));
    FreeLibrary(module);
    return result;
}

/** Runs one remote function and returns its 32-bit thread exit code. */
[[nodiscard]] std::optional<DWORD> RunRemoteFunction(
    HANDLE process, std::uintptr_t address, void* parameter, std::wstring& error) {
    UniqueHandle thread(CreateRemoteThread(
        process, nullptr, 0, reinterpret_cast<LPTHREAD_START_ROUTINE>(address), parameter, 0, nullptr));
    if (!thread) {
        error = L"CreateRemoteThread failed with error " + std::to_wstring(GetLastError());
        return std::nullopt;
    }
    const DWORD wait = WaitForSingleObject(thread.get(), 30'000);
    if (wait != WAIT_OBJECT_0) {
        error = L"Remote function did not finish within 30 seconds";
        return std::nullopt;
    }
    DWORD exitCode = 0;
    if (GetExitCodeThread(thread.get(), &exitCode) == FALSE) {
        error = L"GetExitCodeThread failed with error " + std::to_wstring(GetLastError());
        return std::nullopt;
    }
    return exitCode;
}

/** Verifies that the target executable is a 32-bit Windows binary. */
[[nodiscard]] bool VerifyX86Process(HANDLE process, std::wstring& error) {
    std::vector<wchar_t> path(32768);
    DWORD length = static_cast<DWORD>(path.size());
    if (QueryFullProcessImageNameW(process, 0, path.data(), &length) == FALSE) {
        error = L"QueryFullProcessImageNameW failed";
        return false;
    }
    DWORD binaryType = 0;
    if (GetBinaryTypeW(path.data(), &binaryType) == FALSE || binaryType != SCS_32BIT_BINARY) {
        error = L"Target is not an x86 Windows executable";
        return false;
    }
    return true;
}

/** Best-effort rolls back a loaded DLL after SC13_Initialize reports failure. */
[[nodiscard]] bool RollBackFailedInitialization(
    HANDLE process,
    DWORD processId,
    const std::filesystem::path& dllPath,
    std::uintptr_t remoteLoader,
    std::wstring& error) {
    bool clean = true;
    const auto shutdownRva = ExportRva(dllPath, "SC13_Shutdown");
    std::wstring rollbackError;
    if (!shutdownRva.has_value() ||
        !RunRemoteFunction(
             process, remoteLoader + shutdownRva.value(), nullptr, rollbackError)
             .has_value()) {
        clean = false;
    }
    const auto remoteFreeLibrary =
        ResolveRemoteProcedure(processId, L"kernel32.dll", "FreeLibrary");
    const auto unloadResult = remoteFreeLibrary.has_value() ?
        RunRemoteFunction(
            process, remoteFreeLibrary.value(), reinterpret_cast<void*>(remoteLoader),
            rollbackError) :
        std::nullopt;
    if (!unloadResult.has_value() || unloadResult.value() == 0U) {
        clean = false;
    }
    if (!clean) {
        error += L"; automatic shutdown/unload also failed";
        if (!rollbackError.empty()) {
            error += L": " + rollbackError;
        }
    }
    return clean;
}

/** Loads the DLL, then invokes SC13_Initialize outside DllMain in the target process. */
[[nodiscard]] bool InjectAndInitialize(
    HANDLE process,
    DWORD processId,
    const std::filesystem::path& dllPath,
    std::wstring& error) {
    if (!VerifyX86Process(process, error)) {
        return false;
    }
    std::error_code pathError;
    const std::filesystem::path absoluteDll = std::filesystem::absolute(dllPath, pathError);
    if (pathError || !std::filesystem::is_regular_file(absoluteDll, pathError) || pathError) {
        error = L"Loader DLL does not exist: " + dllPath.wstring();
        return false;
    }
    if (FindRemoteModule(processId, absoluteDll.filename().wstring()).has_value()) {
        error = L"Loader DLL is already present in PID " + std::to_wstring(processId) +
                L"; refusing to increment its LoadLibrary reference count";
        return false;
    }

    const auto remoteLoadLibrary =
        ResolveRemoteProcedure(processId, L"kernel32.dll", "LoadLibraryW");
    if (!remoteLoadLibrary.has_value()) {
        error = L"Could not resolve remote LoadLibraryW";
        return false;
    }

    const std::wstring dllText = absoluteDll.wstring();
    const SIZE_T byteCount = (dllText.size() + 1U) * sizeof(wchar_t);
    void* remoteText = VirtualAllocEx(
        process, nullptr, byteCount, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (remoteText == nullptr) {
        error = L"VirtualAllocEx failed with error " + std::to_wstring(GetLastError());
        return false;
    }
    SIZE_T written = 0;
    const bool writeSucceeded = WriteProcessMemory(
        process, remoteText, dllText.c_str(), byteCount, &written) != FALSE && written == byteCount;
    if (!writeSucceeded) {
        error = L"WriteProcessMemory failed with error " + std::to_wstring(GetLastError());
        VirtualFreeEx(process, remoteText, 0, MEM_RELEASE);
        return false;
    }

    const auto loadResult = RunRemoteFunction(process, remoteLoadLibrary.value(), remoteText, error);
    VirtualFreeEx(process, remoteText, 0, MEM_RELEASE);
    if (!loadResult.has_value() || loadResult.value() == 0) {
        if (error.empty()) {
            error = L"Remote LoadLibraryW returned null";
        }
        return false;
    }

    const std::uintptr_t remoteLoader =
        static_cast<std::uintptr_t>(loadResult.value());
    const auto initializeRva = ExportRva(absoluteDll, "SC13_Initialize");
    if (!initializeRva.has_value()) {
        error = L"Could not resolve SC13_Initialize in the loaded module";
        const auto remoteFreeLibrary =
            ResolveRemoteProcedure(processId, L"kernel32.dll", "FreeLibrary");
        std::wstring unloadError;
        if (remoteFreeLibrary.has_value()) {
            (void)RunRemoteFunction(
                process, remoteFreeLibrary.value(), reinterpret_cast<void*>(remoteLoader),
                unloadError);
        }
        return false;
    }
    const auto initializeResult = RunRemoteFunction(
        process, remoteLoader + initializeRva.value(), nullptr, error);
    if (!initializeResult.has_value()) {
        return false;
    }
    if (initializeResult.value() != 0) {
        error = L"SC13_Initialize returned code " + std::to_wstring(initializeResult.value()) +
                L"; inspect logs/sc13modloader.log";
        (void)RollBackFailedInitialization(
            process, processId, absoluteDll, remoteLoader, error);
        return false;
    }
    return true;
}

/** Invokes SC13_Shutdown and unloads one previously injected loader module. */
[[nodiscard]] bool ShutdownAndUnload(
    HANDLE process,
    DWORD processId,
    const std::filesystem::path& dllPath,
    std::wstring& error) {
    std::error_code pathError;
    const std::filesystem::path absoluteDll = std::filesystem::absolute(dllPath, pathError);
    if (pathError || !std::filesystem::is_regular_file(absoluteDll, pathError) || pathError) {
        error = L"Loader DLL does not exist: " + dllPath.wstring();
        return false;
    }
    const auto remoteLoader = FindRemoteModule(processId, absoluteDll.filename().wstring());
    const auto shutdownRva = ExportRva(absoluteDll, "SC13_Shutdown");
    if (!remoteLoader.has_value() || !shutdownRva.has_value()) {
        error = L"Could not resolve the loaded module or SC13_Shutdown";
        return false;
    }
    const auto shutdownResult = RunRemoteFunction(
        process, remoteLoader.value() + shutdownRva.value(), nullptr, error);
    if (!shutdownResult.has_value() || shutdownResult.value() != 0) {
        if (error.empty()) {
            error = L"SC13_Shutdown returned code " +
                    std::to_wstring(shutdownResult.value_or(MAXDWORD));
        }
        return false;
    }

    const auto remoteFreeLibrary =
        ResolveRemoteProcedure(processId, L"kernel32.dll", "FreeLibrary");
    if (!remoteFreeLibrary.has_value()) {
        error = L"Could not resolve remote FreeLibrary";
        return false;
    }
    const auto unloadResult = RunRemoteFunction(
        process, remoteFreeLibrary.value(),
        reinterpret_cast<void*>(remoteLoader.value()), error);
    if (!unloadResult.has_value() || unloadResult.value() == 0) {
        if (error.empty()) {
            error = L"Remote FreeLibrary returned false";
        }
        return false;
    }
    return true;
}

/** Opens an existing process with only the rights required for DLL injection. */
[[nodiscard]] UniqueHandle OpenTargetProcess(DWORD processId) {
    return UniqueHandle(OpenProcess(
        SYNCHRONIZE | PROCESS_CREATE_THREAD | PROCESS_QUERY_INFORMATION |
            PROCESS_QUERY_LIMITED_INFORMATION | PROCESS_VM_OPERATION | PROCESS_VM_WRITE |
            PROCESS_VM_READ,
        FALSE, processId));
}

}  // namespace

/** Attaches to or starts SimCity and initializes the observational loader DLL. */
int wmain(int argumentCount, wchar_t** arguments) {
    if (argumentCount < 2) {
        PrintUsage();
        return 2;
    }
    const auto defaultDll = DefaultDllPath();
    if (!defaultDll.has_value()) {
        std::wcerr << L"Could not resolve launcher directory.\n";
        return 1;
    }

    const std::wstring_view mode = arguments[1];
    if (mode == L"--self-check") {
        if (argumentCount > 3) {
            PrintUsage();
            return 2;
        }
        const std::filesystem::path dllPath =
            argumentCount == 3 ? arguments[2] : defaultDll.value();
        const auto initializeRva = ExportRva(dllPath, "SC13_Initialize");
        const auto shutdownRva = ExportRva(dllPath, "SC13_Shutdown");
        if (!initializeRva.has_value() || !shutdownRva.has_value()) {
            std::wcerr << L"DLL self-check failed: stable bootstrap exports are missing.\n";
            return 1;
        }
        std::wcout << L"DLL self-check passed. SC13_Initialize RVA=0x" << std::hex
                   << initializeRva.value() << L", SC13_Shutdown RVA=0x" << shutdownRva.value()
                   << L".\n";
        return 0;
    }

    if (mode == L"--attach") {
        if (argumentCount > 4) {
            PrintUsage();
            return 2;
        }
        DWORD processId = 0;
        int dllArgument = 2;
        if (argumentCount >= 3) {
            const auto parsedId = ParseProcessId(arguments[2]);
            if (parsedId.has_value()) {
                processId = parsedId.value();
                dllArgument = 3;
            }
        }
        if (processId == 0) {
            const auto processes = FindSimCityProcesses();
            if (processes.size() != 1) {
                std::wcerr << L"Expected exactly one SimCity.exe process, found "
                           << processes.size() << L". Supply its PID explicitly.\n";
                return 1;
            }
            processId = processes.front();
        }
        const std::filesystem::path dllPath =
            argumentCount > dllArgument ? arguments[dllArgument] : defaultDll.value();
        UniqueHandle process = OpenTargetProcess(processId);
        if (!process) {
            std::wcerr << L"OpenProcess failed with error " << GetLastError() << L".\n";
            return 1;
        }
        std::wstring error;
        if (!InjectAndInitialize(process.get(), processId, dllPath, error)) {
            std::wcerr << L"Injection failed: " << error << L"\n";
            return 1;
        }
        std::wcout << L"Simoder runtime initialized in PID " << processId << L".\n";
        return 0;
    }

    if (mode == L"--watch-attach") {
        if (argumentCount < 3 || argumentCount > 4) {
            PrintUsage();
            return 2;
        }
        const auto excludedProcessId = ParseProcessId(arguments[2]);
        if (!excludedProcessId.has_value()) {
            PrintUsage();
            return 2;
        }
        const std::filesystem::path dllPath =
            argumentCount == 4 ? arguments[3] : defaultDll.value();
        constexpr ULONGLONG timeoutMilliseconds = 10ULL * 60ULL * 1000ULL;
        const ULONGLONG deadline = GetTickCount64() + timeoutMilliseconds;
        DWORD candidateProcessId = 0;
        std::wstring lastError;
        std::map<DWORD, UniqueHandle> attachedCandidates;
        while (GetTickCount64() < deadline) {
            const std::vector<DWORD> processes = FindSimCityProcesses();
            for (auto candidate = attachedCandidates.begin();
                 candidate != attachedCandidates.end();) {
                const bool processAlive =
                    std::find(processes.begin(), processes.end(), candidate->first) !=
                    processes.end();
                if (!processAlive) {
                    candidate = attachedCandidates.erase(candidate);
                    continue;
                }
                const DWORD waitResult = WaitForSingleObject(candidate->second.get(), 0U);
                if (waitResult == WAIT_OBJECT_0) {
                    std::wcout << L"SC13 runtime patch applied in PID "
                               << candidate->first << L".\n";
                    return 0;
                }
                if (waitResult == WAIT_FAILED) {
                    lastError = L"Patch-applied event wait failed for PID " +
                                std::to_wstring(candidate->first) + L" with error " +
                                std::to_wstring(GetLastError());
                }
                ++candidate;
            }
            for (const DWORD processId : processes) {
                if (processId == excludedProcessId.value() ||
                    attachedCandidates.contains(processId)) {
                    continue;
                }
                candidateProcessId = processId;
                UniqueHandle process = OpenTargetProcess(processId);
                if (!process) {
                    lastError = L"OpenProcess failed with error " +
                                std::to_wstring(GetLastError());
                    continue;
                }
                std::wstring error;
                if (InjectAndInitialize(process.get(), processId, dllPath, error)) {
                    const std::wstring signalName =
                        sc13::runtime::PatchAppliedSignalName(processId);
                    UniqueHandle signal(OpenEventW(SYNCHRONIZE, FALSE, signalName.c_str()));
                    if (signal) {
                        attachedCandidates.emplace(processId, std::move(signal));
                        continue;
                    }
                    error = L"Could not open patch-applied event for PID " +
                            std::to_wstring(processId) + L" with error " +
                            std::to_wstring(GetLastError());
                    std::wstring rollbackError;
                    if (!ShutdownAndUnload(
                            process.get(), processId, dllPath, rollbackError) &&
                        !rollbackError.empty()) {
                        error += L"; rollback failed: " + rollbackError;
                    }
                }
                lastError = std::move(error);
            }
            Sleep(50U);
        }
        std::wcerr << L"Timed out waiting for a SimCity process to apply the runtime patch";
        if (candidateProcessId != 0U) {
            std::wcerr << L"; last candidate PID " << candidateProcessId;
        }
        if (!lastError.empty()) {
            std::wcerr << L": " << lastError;
        }
        std::wcerr << L".\n";
        return 1;
    }

    if (mode == L"--detach") {
        if (argumentCount < 3 || argumentCount > 4) {
            PrintUsage();
            return 2;
        }
        const auto processId = ParseProcessId(arguments[2]);
        if (!processId.has_value()) {
            PrintUsage();
            return 2;
        }
        const std::filesystem::path dllPath =
            argumentCount == 4 ? arguments[3] : defaultDll.value();
        UniqueHandle process = OpenTargetProcess(processId.value());
        if (!process) {
            std::wcerr << L"OpenProcess failed with error " << GetLastError() << L".\n";
            return 1;
        }
        std::wstring error;
        if (!ShutdownAndUnload(process.get(), processId.value(), dllPath, error)) {
            std::wcerr << L"Detach failed: " << error << L"\n";
            return 1;
        }
        std::wcout << L"SC13 loader shut down and unloaded from PID "
                   << processId.value() << L".\n";
        return 0;
    }

    if (mode == L"--launch") {
        if (argumentCount < 3 || argumentCount > 4) {
            PrintUsage();
            return 2;
        }
        const std::filesystem::path executable = arguments[2];
        const std::filesystem::path dllPath = argumentCount == 4 ? arguments[3] : defaultDll.value();
        std::wstring commandLine = L"\"" + executable.wstring() + L"\"";
        STARTUPINFOW startup{};
        startup.cb = sizeof(startup);
        PROCESS_INFORMATION processInfo{};
        const std::wstring workingDirectory = executable.parent_path().wstring();
        if (CreateProcessW(
                executable.c_str(), commandLine.data(), nullptr, nullptr, FALSE,
                CREATE_SUSPENDED | DEBUG_ONLY_THIS_PROCESS, nullptr,
                workingDirectory.c_str(), &startup, &processInfo) == FALSE) {
            std::wcerr << L"CreateProcessW failed with error " << GetLastError() << L".\n";
            return 1;
        }
        UniqueHandle process(processInfo.hProcess);
        UniqueHandle primaryThread(processInfo.hThread);
        std::wstring error;
        if (!AdvanceToExecutableEntry(processInfo, error)) {
            TerminateProcess(process.get(), 1);
            std::wcerr << L"Early bootstrap failed; child was terminated: " << error << L"\n";
            return 1;
        }
        if (!InjectAndInitialize(process.get(), processInfo.dwProcessId, dllPath, error)) {
            TerminateProcess(process.get(), 1);
            std::wcerr << L"Early injection failed; suspended child was terminated: " << error << L"\n";
            return 1;
        }
        if (ResumeThread(primaryThread.get()) == static_cast<DWORD>(-1)) {
            TerminateProcess(process.get(), 1);
            std::wcerr << L"ResumeThread failed; child was terminated.\n";
            return 1;
        }
        std::wcout << L"SimCity launched with Simoder runtime in PID "
                   << processInfo.dwProcessId << L".\n";
        return 0;
    }

    PrintUsage();
    return 2;
}
