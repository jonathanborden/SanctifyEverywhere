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
        // reserved != nullptr -> process is terminating (the normal game-exit
        // path). Never wait or unload libraries there; it only delays exit.
        Mod::Stop(reserved != nullptr);
        if (!reserved) Proxy::Shutdown();
        break;
    }
    return TRUE;
}
