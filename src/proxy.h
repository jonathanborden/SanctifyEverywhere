#pragma once
#include <windows.h>

// dwmapi.dll proxy - forwards all exports to the real DLL
namespace Proxy {
    void Init();
    void Shutdown();
}
