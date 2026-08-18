<div align="center">

<img src="assets/ring0.ico" width="96" alt="ring0">

# ring0

**native win32 task manager. no CRT, sub-50KB packed with crinkler.**

[![license](https://img.shields.io/badge/license-GPLv3-d6262a?style=flat-square)](LICENSE)
![platform](https://img.shields.io/badge/platform-Windows%2010%2F11%20x64-d6262a?style=flat-square)

</div>

---

native win32 task manager. no CRT, sub-50KB packed with crinkler.
win10 21H2+ / win11, x64.

## build

open "x64 native tools command prompt for VS 2022", then:

    build.bat debug
    build.bat release

crinkler is optional. drop `crinkler.exe` in `tools\crinkler\` for packed release builds.

## status

phase 0. blank window only.
