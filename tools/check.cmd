@echo off
rem Compile-check one or more sources in isolation with the project's flags.
rem usage: tools\check.cmd src\core\cpu_sampler.cpp [more.cpp ...]
rem Does not link and does not run moc; safe to run while other files are mid-edit.
setlocal
if not defined VCINSTALLDIR call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat" >nul
set Q=C:\Qt\6.11.3\msvc2022_64\include
set ROOT=%~dp0..
if not exist "%TEMP%\ring0-check" mkdir "%TEMP%\ring0-check"
cl /nologo /c /std:c++latest /EHsc /W4 /WX /permissive- /utf-8 /Zc:__cplusplus /MDd ^
   /DUNICODE /D_UNICODE /DWIN32_LEAN_AND_MEAN /DNOMINMAX ^
   /I"%ROOT%\src" /I"%ROOT%\src\app" /I"%ROOT%\src\gpu" ^
   /external:W0 /external:I"%Q%" /external:I"%Q%\QtCore" /external:I"%Q%\QtGui" ^
   /external:I"%Q%\QtQml" /external:I"%Q%\QtQmlIntegration" /external:I"%Q%\QtQuick" ^
   /Fo"%TEMP%\ring0-check\\" %*
