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

static LRESULT CALLBACK HookWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (msg == WM_TIMER && wParam == 42069) {
        TickHook::TickCallback cb = (TickHook::TickCallback)InterlockedExchangePointer(
            (volatile PVOID*)&gPendingCallback, nullptr);
        if (cb) {
            Log("[Timer] Callback firing on thread %lu\n", GetCurrentThreadId());
            cb();
        }
        return 0;
    }
    return CallWindowProcW(gOriginalWndProc, hwnd, msg, wParam, lParam);
}

bool TickHook::Install(uintptr_t unused) {
    (void)unused;

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
        return false;
    }

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
    while (InterlockedCompareExchangePointer((volatile PVOID*)&gPendingCallback, (PVOID)cb, nullptr) != nullptr) {
        Sleep(1);
    }
}
