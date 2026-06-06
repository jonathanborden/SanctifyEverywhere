#pragma once
#include <windows.h>

// Diagnostic logging master switch. 0 = silent (release default, no log files
// are created). Set to 1 to write SanctifyMod.log / SanctifyMod_proxy.log next
// to the deployed DLL (the game's Win64 folder — never a hard-coded user path).
#define SANCTIFY_LOGGING 0

namespace Mod {
    void Start();
    void Stop();
}
