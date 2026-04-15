@echo off
setlocal enableextensions enabledelayedexpansion

rem ring0 build driver. no msbuild, no cmake.
rem usage: build.bat debug | release

set MODE=%~1
if "%MODE%"=="" set MODE=debug
if /I not "%MODE%"=="debug" if /I not "%MODE%"=="release" (
    echo unknown mode: %MODE%
    echo usage: build.bat [debug^|release]
    exit /b 2
)

if not defined VCINSTALLDIR (
    echo error: run from "x64 native tools command prompt for VS 2022".
    echo ^(or source vcvarsall.bat x64 first^)
    exit /b 3
)

if not exist build mkdir build

set CL_COMMON=/c /O1 /Os /GS- /Gs9999999 /GR- /EHs-c- /Zl /W4 /WX /nologo /DUNICODE /D_UNICODE /Isrc
if /I "%MODE%"=="debug"   set CL_MODE=/Zi /DDEBUG /Fdbuild\ring0.pdb
if /I "%MODE%"=="release" set CL_MODE=/DNDEBUG

set OBJS=
for %%f in (src\*.c src\*.cpp) do (
    echo cl %%f
    cl %CL_COMMON% %CL_MODE% %%f /Fobuild\%%~nf.obj
    if errorlevel 1 exit /b 1
    set OBJS=!OBJS! build\%%~nf.obj
)

set LINK_COMMON=/SUBSYSTEM:WINDOWS,6.00 /ENTRY:WinMainCRTStartup /NODEFAULTLIB /MERGE:.rdata=.text /MERGE:.pdata=.text /ALIGN:16 /MANIFEST:EMBED /MANIFESTUAC:NO /MANIFESTINPUT:res\ring0.manifest /OPT:REF /OPT:ICF=4 /STACK:0x100000,0x100000 /OUT:build\ring0.exe /nologo
set LINK_LIBS=kernel32.lib user32.lib gdi32.lib ntdll.lib d2d1.lib dwrite.lib dxgi.lib d3d11.lib dwmapi.lib advapi32.lib wintrust.lib
if /I "%MODE%"=="debug" set LINK_MODE=/DEBUG /PDB:build\ring0.pdb

echo link
link %LINK_COMMON% %LINK_MODE% %OBJS% %LINK_LIBS%
if errorlevel 1 exit /b 1

if /I "%MODE%"=="release" (
    if exist tools\crinkler\crinkler.exe (
        echo crinkler
        tools\crinkler\crinkler.exe %LINK_COMMON% %OBJS% %LINK_LIBS% /OUT:build\ring0.exe
        if errorlevel 1 exit /b 1
    ) else (
        echo warning: tools\crinkler\crinkler.exe not found. release binary is msvc-linked, not packed.
    )
)

for %%s in (build\ring0.exe) do echo built: %%s  %%~zs bytes
endlocal
