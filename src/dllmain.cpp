#include "proxy.h"
#include "mod.h"

// Our own module handle — used by Log() to write next to the deployed DLL
// instead of a hard-coded Desktop path.
HMODULE gThisModule = nullptr;

BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID reserved) {
    switch (reason) {
    case DLL_PROCESS_ATTACH:
        gThisModule = hModule;
        DisableThreadLibraryCalls(hModule);
        Proxy::Init();
        // Do NOT start mod thread here - DllMain holds the loader lock
        // Use a delayed start instead
        break;
    case DLL_PROCESS_DETACH:
        Mod::Stop();
        Proxy::Shutdown();
        break;
    }
    return TRUE;
}
