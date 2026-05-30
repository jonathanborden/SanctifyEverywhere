@echo off
setlocal

:: Find Visual Studio
for /f "usebackq tokens=*" %%i in (`"%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" -latest -property installationPath`) do set VSDIR=%%i
call "%VSDIR%\VC\Auxiliary\Build\vcvars64.bat"

:: Build the DLL
cd /d "%~dp0"
if not exist build mkdir build

cl.exe /nologo /O2 /LD /EHsc /std:c++17 ^
    /Isrc ^
    src\dllmain.cpp src\proxy.cpp src\mod.cpp src\tick_hook.cpp ^
    /Fe:build\dwmapi.dll ^
    /link /DLL /DEF:src\dwmapi.def ^
    user32.lib kernel32.lib

if %ERRORLEVEL% EQU 0 (
    echo.
    echo === BUILD SUCCEEDED ===
    echo Output: build\dwmapi.dll
    echo.
    echo To install: copy build\dwmapi.dll to the game's Win64 folder
) else (
    echo.
    echo === BUILD FAILED ===
)

pause
