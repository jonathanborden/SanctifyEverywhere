#include "proxy.h"
#include "mod.h"
#include <cstdio>

static HMODULE hRealDwmapi = nullptr;
static bool gModStarted = false;
static FILE* gProxyLog = nullptr;
static DWORD gProxyLogLastTick = 0;

extern HMODULE gThisModule; // set in dllmain.cpp

static void ProxyLog(const char* fmt, ...) {
    if (!gProxyLog) {
        // Write next to the deployed DLL (game's Win64 folder).
        char path[MAX_PATH] = {};
        if (gThisModule && GetModuleFileNameA(gThisModule, path, MAX_PATH)) {
            char* slash = strrchr(path, '\\');
            if (slash) *(slash + 1) = 0;
            strcat_s(path, "SanctifyMod_proxy.log");
        } else {
            strcpy_s(path, ".\\SanctifyMod_proxy.log");
        }
        gProxyLog = fopen(path, "w");
    }
    if (!gProxyLog) return;
    SYSTEMTIME st; GetLocalTime(&st);
    DWORD now = GetTickCount();
    DWORD delta = gProxyLogLastTick ? (now - gProxyLogLastTick) : 0;
    gProxyLogLastTick = now;
    fprintf(gProxyLog, "[%02u:%02u:%02u.%03u +%lums] ",
        st.wHour, st.wMinute, st.wSecond, st.wMilliseconds, (unsigned long)delta);
    va_list args;
    va_start(args, fmt);
    vfprintf(gProxyLog, fmt, args);
    va_end(args);
    fflush(gProxyLog);
}

// Generic function pointer storage - index by ordinal/name
#define MAX_EXPORTS 64
static FARPROC realFuncs[MAX_EXPORTS] = {};

// Map of function names to our array indices
static const char* funcNames[] = {
    "DwmIsCompositionEnabled",        // 0
    "DwmEnableBlurBehindWindow",      // 1
    "DwmExtendFrameIntoClientArea",   // 2
    "DwmGetColorizationColor",        // 3
    "DwmGetWindowAttribute",          // 4
    "DwmSetWindowAttribute",          // 5
    "DwmFlush",                       // 6
    "DwmDefWindowProc",               // 7
    "DwmRegisterThumbnail",           // 8
    "DwmUnregisterThumbnail",         // 9
    "DwmUpdateThumbnailProperties",   // 10
    "DwmQueryThumbnailSourceSize",    // 11
    "DwmEnableComposition",           // 12
    "DwmGetTransportAttributes",      // 13
    "DwmSetPresentParameters",        // 14
    "DwmInvalidateIconicBitmaps",     // 15
    "DwmSetIconicThumbnail",          // 16
    "DwmSetIconicLivePreviewBitmap",  // 17
    "DwmGetGraphicsStreamTransformHint", // 18
    "DwmGetGraphicsStreamClient",     // 19
    "DwmEnableMMCSS",                 // 20
    "DwmAttachMilContent",            // 21
    "DwmDetachMilContent",            // 22
    "DwmModifyPreviousDxFrameDuration", // 23
    "DwmSetDxFrameDuration",          // 24
    "DwmGetCompositionTimingInfo",    // 25
    nullptr
};

static void EnsureModStarted() {
    if (!gModStarted) {
        gModStarted = true;
        ProxyLog("[Proxy] First dwmapi call - starting mod thread\n");
        Mod::Start();
    }
}

void Proxy::Init() {
    ProxyLog("[Proxy] Init called! DLL is loaded.\n");

    char syspath[MAX_PATH];
    GetSystemDirectoryA(syspath, MAX_PATH);
    strcat_s(syspath, "\\dwmapi.dll");

    ProxyLog("[Proxy] Loading real dwmapi from: %s\n", syspath);
    hRealDwmapi = LoadLibraryA(syspath);
    if (!hRealDwmapi) {
        ProxyLog("[Proxy] FAILED to load real dwmapi.dll! Error: %lu\n", GetLastError());
        return;
    }

    ProxyLog("[Proxy] Real dwmapi loaded at 0x%p\n", hRealDwmapi);

    for (int i = 0; funcNames[i] != nullptr && i < MAX_EXPORTS; i++) {
        realFuncs[i] = GetProcAddress(hRealDwmapi, funcNames[i]);
        if (realFuncs[i]) ProxyLog("[Proxy] Found: %s\n", funcNames[i]);
    }

    ProxyLog("[Proxy] Init complete, starting mod thread...\n");
    Mod::Start();
    ProxyLog("[Proxy] Mod::Start() called\n");
}

void Proxy::Shutdown() {
    if (hRealDwmapi) {
        FreeLibrary(hRealDwmapi);
        hRealDwmapi = nullptr;
    }
}

// All exported functions forward to the real DLL and trigger mod start on first call
extern "C" {

__declspec(dllexport) HRESULT __stdcall DwmIsCompositionEnabled(BOOL* pfEnabled) {
    EnsureModStarted();
    if (realFuncs[0]) return ((HRESULT(__stdcall*)(BOOL*))realFuncs[0])(pfEnabled);
    if (pfEnabled) *pfEnabled = TRUE;
    return S_OK;
}

__declspec(dllexport) HRESULT __stdcall DwmEnableBlurBehindWindow(HWND hWnd, const void* pBlurBehind) {
    EnsureModStarted();
    if (realFuncs[1]) return ((HRESULT(__stdcall*)(HWND,const void*))realFuncs[1])(hWnd, pBlurBehind);
    return S_OK;
}

__declspec(dllexport) HRESULT __stdcall DwmExtendFrameIntoClientArea(HWND hWnd, const void* pMarInset) {
    EnsureModStarted();
    if (realFuncs[2]) return ((HRESULT(__stdcall*)(HWND,const void*))realFuncs[2])(hWnd, pMarInset);
    return S_OK;
}

__declspec(dllexport) HRESULT __stdcall DwmGetColorizationColor(DWORD* pcrColorization, BOOL* pfOpaqueBlend) {
    EnsureModStarted();
    if (realFuncs[3]) return ((HRESULT(__stdcall*)(DWORD*,BOOL*))realFuncs[3])(pcrColorization, pfOpaqueBlend);
    return E_NOTIMPL;
}

__declspec(dllexport) HRESULT __stdcall DwmGetWindowAttribute(HWND hwnd, DWORD attr, PVOID pv, DWORD cb) {
    EnsureModStarted();
    if (realFuncs[4]) return ((HRESULT(__stdcall*)(HWND,DWORD,PVOID,DWORD))realFuncs[4])(hwnd, attr, pv, cb);
    return E_NOTIMPL;
}

__declspec(dllexport) HRESULT __stdcall DwmSetWindowAttribute(HWND hwnd, DWORD attr, LPCVOID pv, DWORD cb) {
    EnsureModStarted();
    if (realFuncs[5]) return ((HRESULT(__stdcall*)(HWND,DWORD,LPCVOID,DWORD))realFuncs[5])(hwnd, attr, pv, cb);
    return E_NOTIMPL;
}

__declspec(dllexport) HRESULT __stdcall DwmFlush() {
    EnsureModStarted();
    if (realFuncs[6]) return ((HRESULT(__stdcall*)())realFuncs[6])();
    return S_OK;
}

__declspec(dllexport) BOOL __stdcall DwmDefWindowProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam, LRESULT* plResult) {
    EnsureModStarted();
    if (realFuncs[7]) return ((BOOL(__stdcall*)(HWND,UINT,WPARAM,LPARAM,LRESULT*))realFuncs[7])(hWnd, msg, wParam, lParam, plResult);
    return FALSE;
}

__declspec(dllexport) HRESULT __stdcall DwmRegisterThumbnail(HWND hwndDest, HWND hwndSrc, void** phThumbnail) {
    if (realFuncs[8]) return ((HRESULT(__stdcall*)(HWND,HWND,void**))realFuncs[8])(hwndDest, hwndSrc, phThumbnail);
    return E_NOTIMPL;
}

__declspec(dllexport) HRESULT __stdcall DwmUnregisterThumbnail(void* hThumbnail) {
    if (realFuncs[9]) return ((HRESULT(__stdcall*)(void*))realFuncs[9])(hThumbnail);
    return E_NOTIMPL;
}

__declspec(dllexport) HRESULT __stdcall DwmUpdateThumbnailProperties(void* hThumbnail, const void* ptnProperties) {
    if (realFuncs[10]) return ((HRESULT(__stdcall*)(void*,const void*))realFuncs[10])(hThumbnail, ptnProperties);
    return E_NOTIMPL;
}

__declspec(dllexport) HRESULT __stdcall DwmQueryThumbnailSourceSize(void* hThumbnail, void* pSize) {
    if (realFuncs[11]) return ((HRESULT(__stdcall*)(void*,void*))realFuncs[11])(hThumbnail, pSize);
    return E_NOTIMPL;
}

__declspec(dllexport) HRESULT __stdcall DwmEnableComposition(UINT uCompositionAction) {
    if (realFuncs[12]) return ((HRESULT(__stdcall*)(UINT))realFuncs[12])(uCompositionAction);
    return S_OK;
}

__declspec(dllexport) HRESULT __stdcall DwmGetTransportAttributes(BOOL* pfIsRemoting, BOOL* pfIsConnected, DWORD* pDwGeneration) {
    if (realFuncs[13]) return ((HRESULT(__stdcall*)(BOOL*,BOOL*,DWORD*))realFuncs[13])(pfIsRemoting, pfIsConnected, pDwGeneration);
    return E_NOTIMPL;
}

__declspec(dllexport) HRESULT __stdcall DwmSetPresentParameters(HWND hwnd, void* pPresentParams) {
    if (realFuncs[14]) return ((HRESULT(__stdcall*)(HWND,void*))realFuncs[14])(hwnd, pPresentParams);
    return E_NOTIMPL;
}

__declspec(dllexport) HRESULT __stdcall DwmInvalidateIconicBitmaps(HWND hwnd) {
    if (realFuncs[15]) return ((HRESULT(__stdcall*)(HWND))realFuncs[15])(hwnd);
    return E_NOTIMPL;
}

__declspec(dllexport) HRESULT __stdcall DwmSetIconicThumbnail(HWND hwnd, HBITMAP hbmp, DWORD dwSITFlags) {
    if (realFuncs[16]) return ((HRESULT(__stdcall*)(HWND,HBITMAP,DWORD))realFuncs[16])(hwnd, hbmp, dwSITFlags);
    return E_NOTIMPL;
}

__declspec(dllexport) HRESULT __stdcall DwmSetIconicLivePreviewBitmap(HWND hwnd, HBITMAP hbmp, void* pptClient, DWORD dwSITFlags) {
    if (realFuncs[17]) return ((HRESULT(__stdcall*)(HWND,HBITMAP,void*,DWORD))realFuncs[17])(hwnd, hbmp, pptClient, dwSITFlags);
    return E_NOTIMPL;
}

__declspec(dllexport) HRESULT __stdcall DwmGetGraphicsStreamTransformHint(UINT uIndex, void* pTransform) {
    if (realFuncs[18]) return ((HRESULT(__stdcall*)(UINT,void*))realFuncs[18])(uIndex, pTransform);
    return E_NOTIMPL;
}

__declspec(dllexport) HRESULT __stdcall DwmGetGraphicsStreamClient(UINT uIndex, void* pClientUuid) {
    if (realFuncs[19]) return ((HRESULT(__stdcall*)(UINT,void*))realFuncs[19])(uIndex, pClientUuid);
    return E_NOTIMPL;
}

__declspec(dllexport) HRESULT __stdcall DwmEnableMMCSS(BOOL fEnableMMCSS) {
    if (realFuncs[20]) return ((HRESULT(__stdcall*)(BOOL))realFuncs[20])(fEnableMMCSS);
    return S_OK;
}

__declspec(dllexport) HRESULT __stdcall DwmGetCompositionTimingInfo(HWND hwnd, void* pTimingInfo) {
    if (realFuncs[25]) return ((HRESULT(__stdcall*)(HWND,void*))realFuncs[25])(hwnd, pTimingInfo);
    return E_NOTIMPL;
}

} // extern "C"
