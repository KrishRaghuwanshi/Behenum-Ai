@echo off
setlocal

set MSVC=C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Tools\MSVC\14.44.35207
set SDK=C:\Program Files (x86)\Windows Kits\10
set SDKV=10.0.26100.0
set SRC=C:\Users\Krish Raghuwanshi\Work\Baihan-Ai\chimera-project\source\cpp
set CL="%MSVC%\bin\Hostx64\x64\cl.exe"
set LK="%MSVC%\bin\Hostx64\x64\link.exe"

set INCS=/I"%MSVC%\include" /I"%SDK%\Include\%SDKV%\ucrt" /I"%SDK%\Include\%SDKV%\um" /I"%SDK%\Include\%SDKV%\shared"
set LIBS=/LIBPATH:"%MSVC%\lib\x64" /LIBPATH:"%SDK%\Lib\%SDKV%\ucrt\x64" /LIBPATH:"%SDK%\Lib\%SDKV%\um\x64"

cd /d "%SRC%"

echo === Compiling DLL sources ===
%CL% /nologo /O2 /std:c++17 /EHa /MT /W3 /D_USRDLL /D_WINDLL /c %INCS% ChimeraMain.cpp NvidiaApi.cpp ScreenCapture.cpp OverlayWindow.cpp OverlayUi.cpp
if errorlevel 1 (echo DLL COMPILE FAILED & goto :eof)

echo === Linking DLL ===
%LK% /nologo /DLL /OUT:ChimeraMain.dll ChimeraMain.obj NvidiaApi.obj ScreenCapture.obj OverlayWindow.obj OverlayUi.obj %LIBS% user32.lib gdi32.lib kernel32.lib ole32.lib windowscodecs.lib winhttp.lib psapi.lib advapi32.lib crypt32.lib
if errorlevel 1 (echo DLL LINK FAILED & goto :eof)
echo DLL: OK

echo === Compiling Bootstrap ===
%CL% /nologo /O2 /std:c++17 /EHa /MT /W3 %INCS% ChimeraBootstrap.cpp
if errorlevel 1 (echo BOOTSTRAP COMPILE FAILED & goto :eof)

echo === Linking Bootstrap ===
%LK% /nologo /SUBSYSTEM:CONSOLE /NODEFAULTLIB /OUT:ChimeraBootstrap.exe ChimeraBootstrap.obj %LIBS% libcmt.lib libvcruntime.lib libucrt.lib user32.lib kernel32.lib psapi.lib ntdll.lib advapi32.lib
if errorlevel 1 (echo BOOTSTRAP LINK FAILED & goto :eof)
echo Bootstrap: OK

echo === Deploying to C:\ChimeraRun\ ===
if not exist C:\ChimeraRun mkdir C:\ChimeraRun
copy /y ChimeraMain.dll C:\ChimeraRun\ChimeraMain.dll
copy /y ChimeraBootstrap.exe C:\ChimeraRun\ChimeraBootstrap.exe
echo Deploy: OK

:eof
endlocal
