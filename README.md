# ring0

native win32 task manager. no CRT, sub-50KB packed with crinkler.
win10 21H2+ / win11, x64.

## build

open "x64 native tools command prompt for VS 2022", then:

    build.bat debug
    build.bat release

crinkler is optional. drop `crinkler.exe` in `tools\crinkler\` for packed release builds.

## status

phase 0. blank window only.
