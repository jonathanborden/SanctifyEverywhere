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
}
