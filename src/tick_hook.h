#pragma once
#include <windows.h>
#include <cstdint>
#include <atomic>

namespace TickHook {
    bool Install(uintptr_t unused);
    bool InstallVtableHook(void* targetObject, uintptr_t processEventAddr, int vtableSlot);
    void Uninstall();
    typedef void (*TickCallback)();
    void QueueOnGameThread(TickCallback cb);

    // Shutdown detection (see tick_hook.cpp). IsShuttingDown(): WM_DESTROY /
    // WM_ENDSESSION was seen — permanent. HeartbeatAgeMs(): ms since the game
    // thread last pumped our WM_TIMER; large = teardown or a long hitch, and
    // UObjects must not be touched.
    bool IsShuttingDown();
    DWORD HeartbeatAgeMs();
}
