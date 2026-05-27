#include "proxy.h"
#include "mod.h"

BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID reserved) {
    switch (reason) {
    case DLL_PROCESS_ATTACH:
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
