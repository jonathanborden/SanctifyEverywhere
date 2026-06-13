#pragma once
#include <windows.h>

// Diagnostic logging master switch. 0 = silent (release default, no log files
// are created). Set to 1 to write SanctifyMod.log / SanctifyMod_proxy.log next
// to the deployed DLL (the game's Win64 folder — never a hard-coded user path).
#define SANCTIFY_LOGGING 1  // TEMP: co-op sanctify diagnostic build — revert to 0 for release

namespace Mod {
    void Start();
    // processTerminating = true when called from DLL_PROCESS_DETACH at process
    // exit (DllMain's reserved != nullptr): threads are already dead and the
    // loader lock is held, so Stop must not wait on the mod thread.
    void Stop(bool processTerminating);
}
