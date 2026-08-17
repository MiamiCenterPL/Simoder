#pragma once

#include <Windows.h>

namespace sc13::bootstrap {

/** Records the DLL module handle without performing work under the loader lock. */
void SetModuleHandle(HMODULE module) noexcept;

}  // namespace sc13::bootstrap

extern "C" {

/** Initializes SC13 Mod Loader from a remote thread after LoadLibraryW returns. */
DWORD WINAPI SC13_Initialize(void* parameter) noexcept;

/** Disables hooks and flushes logging before an intentional unload. */
DWORD WINAPI SC13_Shutdown(void* parameter) noexcept;

}
