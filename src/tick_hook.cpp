#include "tick_hook.h"
#include <cstring>
#include <cstdio>

// PURE WINDOW TIMER — this was the approach in the only build that produced a visible anvil.
// No vtable hooks, no code patching.

extern FILE* gLogFile;
extern void Log(const char* fmt, ...);

static HWND gGameWindow = nullptr;
static UINT_PTR gTimerId = 0;
static WNDPROC gOriginalWndProc = nullptr;
static volatile TickHook::TickCallback gPendingCallback = nullptr;

// Shutdown detection: every WM_TIMER tick stamps a heartbeat. When the game
// thread stops pumping messages (engine teardown after Exit is clicked), the
// heartbeat goes stale and the mod thread must stop touching UObjects.
// WM_DESTROY / WM_ENDSESSION set a permanent flag — the window is going away,
// the mod is done for this process.
static volatile DWORD gLastHeartbeat = 0;
static volatile LONG gShutdownSeen = 0;

static LRESULT CALLBACK HookWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (msg == WM_TIMER && wParam == 42069) {
        gLastHeartbeat = GetTickCount();
        TickHook::TickCallback cb = (TickHook::TickCallback)InterlockedExchangePointer(
            (volatile PVOID*)&gPendingCallback, nullptr);
        if (cb && !gShutdownSeen) { // never run callbacks into a dying engine
            Log("[Timer] Callback firing on thread %lu\n", GetCurrentThreadId());
            cb();
        }
        return 0;
    }
    if (msg == WM_DESTROY || msg == WM_ENDSESSION) {
        InterlockedExchange(&gShutdownSeen, 1);
    }
    return CallWindowProcW(gOriginalWndProc, hwnd, msg, wParam, lParam);
}

bool TickHook::Install(uintptr_t unused) {
    (void)unused;

    // Idempotent: reuse the live subclass if the window is still valid.
    // Repeated uninstall/reinstall churned the wndproc chain for no benefit.
    if (gOriginalWndProc && gGameWindow && IsWindow(gGameWindow)) return true;
    gOriginalWndProc = nullptr; gTimerId = 0; gGameWindow = nullptr;

    gGameWindow = FindWindowA("UnrealWindow", nullptr);
    if (!gGameWindow) {
        Log("[Timer] No UnrealWindow found\n");
        return false;
    }

    gOriginalWndProc = (WNDPROC)SetWindowLongPtrW(gGameWindow, GWLP_WNDPROC, (LONG_PTR)HookWndProc);
    if (!gOriginalWndProc) {
        Log("[Timer] SetWindowLongPtrW failed, error=%lu\n", GetLastError());
        return false;
    }

    gTimerId = SetTimer(gGameWindow, 42069, 100, nullptr);
    if (!gTimerId) {
        Log("[Timer] SetTimer failed\n");
        SetWindowLongPtrW(gGameWindow, GWLP_WNDPROC, (LONG_PTR)gOriginalWndProc);
        gOriginalWndProc = nullptr;
        return false;
    }

    gLastHeartbeat = GetTickCount();
    Log("[Timer] Installed on HWND=0x%llX, timer=%llu\n", (uintptr_t)gGameWindow, (uintptr_t)gTimerId);
    return true;
}

bool TickHook::InstallVtableHook(void* obj, uintptr_t pe, int slot) {
    (void)obj; (void)pe; (void)slot;
    return false; // Disabled — vtable hooks don't produce visible actors
}

void TickHook::Uninstall() {
    if (gTimerId && gGameWindow) {
        KillTimer(gGameWindow, gTimerId);
        gTimerId = 0;
    }
    if (gOriginalWndProc && gGameWindow) {
        SetWindowLongPtrW(gGameWindow, GWLP_WNDPROC, (LONG_PTR)gOriginalWndProc);
        gOriginalWndProc = nullptr;
    }
    gGameWindow = nullptr;
}

void TickHook::QueueOnGameThread(TickCallback cb) {
    // Bounded: if a previous callback is stuck pending (game thread no longer
    // pumping), give up after ~2s instead of spinning forever.
    for (int i = 0; i < 2000; i++) {
        if (InterlockedCompareExchangePointer((volatile PVOID*)&gPendingCallback, (PVOID)cb, nullptr) == nullptr)
            return;
        Sleep(1);
    }
    Log("[Timer] QueueOnGameThread gave up — previous callback never consumed\n");
}

bool TickHook::IsShuttingDown() {
    return gShutdownSeen != 0;
}

DWORD TickHook::HeartbeatAgeMs() {
    if (!gOriginalWndProc) return 0; // hook not installed — no signal, treat as fresh
    DWORD h = gLastHeartbeat;
    return h ? (GetTickCount() - h) : 0;
}
