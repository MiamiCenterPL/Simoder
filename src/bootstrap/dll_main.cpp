#include "bootstrap/loader.hpp"

#include <Windows.h>

/** Performs only loader-lock-safe bookkeeping; initialization is an explicit export. */
BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(instance);
        sc13::bootstrap::SetModuleHandle(instance);
    }
    return TRUE;
}
