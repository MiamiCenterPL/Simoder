#include "reverse/game_build.hpp"

#include <Windows.h>
#include <TlHelp32.h>

#include <array>
#include <cstdio>
#include <span>
#include <vector>

namespace sc13::reverse {
namespace {

/** Reads the executable file version resource as four numeric components. */
[[nodiscard]] bool ReadFileVersion(
    const std::filesystem::path& path, std::string& version, std::string& error) {
    DWORD ignored = 0;
    const DWORD size = GetFileVersionInfoSizeW(path.c_str(), &ignored);
    if (size == 0) {
        error = "GetFileVersionInfoSizeW failed";
        return false;
    }
    std::vector<std::byte> data(size);
    if (GetFileVersionInfoW(path.c_str(), 0, size, data.data()) == FALSE) {
        error = "GetFileVersionInfoW failed";
        return false;
    }
    VS_FIXEDFILEINFO* info = nullptr;
    UINT infoSize = 0;
    if (VerQueryValueW(data.data(), L"\\", reinterpret_cast<void**>(&info), &infoSize) == FALSE ||
        info == nullptr || infoSize < sizeof(VS_FIXEDFILEINFO)) {
        error = "VerQueryValueW did not return VS_FIXEDFILEINFO";
        return false;
    }
    char buffer[64]{};
    const int length = std::snprintf(
        buffer, sizeof(buffer), "%u.%u.%u.%u", HIWORD(info->dwFileVersionMS),
        LOWORD(info->dwFileVersionMS), HIWORD(info->dwFileVersionLS), LOWORD(info->dwFileVersionLS));
    if (length <= 0) {
        error = "Could not format the file version";
        return false;
    }
    version.assign(buffer, static_cast<std::size_t>(length));
    return true;
}

/** Returns the absolute path of the main executable. */
[[nodiscard]] bool ReadExecutablePath(std::filesystem::path& path, std::string& error) {
    std::vector<wchar_t> buffer(32768);
    const DWORD length = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
    if (length == 0 || length == buffer.size()) {
        error = "GetModuleFileNameW failed for the process executable";
        return false;
    }
    path.assign(buffer.data(), buffer.data() + length);
    return true;
}

}  // namespace

bool InspectCurrentBuild(BuildFingerprint& fingerprint, std::string& error) noexcept {
    try {
        fingerprint = {};
        if (!ReadExecutablePath(fingerprint.executablePath, error) ||
            !ReadFileVersion(fingerprint.executablePath, fingerprint.fileVersion, error) ||
            !core::Sha256File(fingerprint.executablePath, fingerprint.fileSha256, error)) {
            return false;
        }

        const HMODULE module = GetModuleHandleW(nullptr);
        if (module == nullptr) {
            error = "GetModuleHandleW returned no executable module";
            return false;
        }
        const auto* base = reinterpret_cast<const std::byte*>(module);
        const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
        if (dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew <= 0) {
            error = "Loaded executable has an invalid DOS header";
            return false;
        }
        const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS32*>(base + dos->e_lfanew);
        if (nt->Signature != IMAGE_NT_SIGNATURE ||
            nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR32_MAGIC) {
            error = "Loaded executable is not a valid PE32 image";
            return false;
        }

        fingerprint.machine = nt->FileHeader.Machine;
        fingerprint.timestamp = nt->FileHeader.TimeDateStamp;
        fingerprint.imageSize = nt->OptionalHeader.SizeOfImage;
        const IMAGE_SECTION_HEADER* sections = IMAGE_FIRST_SECTION(nt);
        const IMAGE_SECTION_HEADER* text = nullptr;
        for (std::uint16_t index = 0; index < nt->FileHeader.NumberOfSections; ++index) {
            if (std::memcmp(sections[index].Name, ".text", 5) == 0) {
                text = &sections[index];
                break;
            }
        }
        if (text == nullptr) {
            error = "Loaded executable has no .text section";
            return false;
        }
        fingerprint.textRva = text->VirtualAddress;
        fingerprint.textSize = text->Misc.VirtualSize;
        if (fingerprint.textRva > fingerprint.imageSize ||
            fingerprint.textSize > fingerprint.imageSize - fingerprint.textRva) {
            error = "Loaded .text section lies outside SizeOfImage";
            return false;
        }
        return core::Sha256(
            std::span<const std::byte>(base + fingerprint.textRva, fingerprint.textSize),
            fingerprint.loadedTextSha256, error);
    } catch (...) {
        error = "Build fingerprinting failed due to an allocation or filesystem exception";
        return false;
    }
}

bool EnumerateCurrentModules(std::vector<ModuleInfo>& modules, std::string& error) noexcept {
    try {
        modules.clear();
        const HANDLE snapshot = CreateToolhelp32Snapshot(
            TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, GetCurrentProcessId());
        if (snapshot == INVALID_HANDLE_VALUE) {
            error = "CreateToolhelp32Snapshot failed for modules";
            return false;
        }

        MODULEENTRY32W entry{};
        entry.dwSize = sizeof(entry);
        if (Module32FirstW(snapshot, &entry) == FALSE) {
            CloseHandle(snapshot);
            error = "Module32FirstW failed";
            return false;
        }
        do {
            modules.push_back(ModuleInfo{
                entry.szExePath,
                reinterpret_cast<std::uintptr_t>(entry.modBaseAddr),
                entry.modBaseSize});
        } while (Module32NextW(snapshot, &entry) != FALSE);
        CloseHandle(snapshot);
        return true;
    } catch (...) {
        error = "Module enumeration failed due to an allocation exception";
        modules.clear();
        return false;
    }
}

}  // namespace sc13::reverse
