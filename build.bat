@echo off
rem SPDX-License-Identifier: AGPL-3.0-or-later
rem Builds mp3toxwm.exe. Needs a C23 compiler: clang 19+ (LLVM) or gcc 13+ (MinGW-w64).
setlocal
cd /d "%~dp0"

set "CC_BIN="
where clang >nul 2>nul && set "CC_BIN=clang"
if not defined CC_BIN if exist "%ProgramFiles%\LLVM\bin\clang.exe" set "CC_BIN=%ProgramFiles%\LLVM\bin\clang.exe"
if not defined CC_BIN where gcc >nul 2>nul && set "CC_BIN=gcc"
if not defined CC_BIN (
    echo No C23 compiler found. Install LLVM clang 19+ or MinGW-w64 gcc 13+.
    exit /b 1
)

setlocal enabledelayedexpansion
set "SRC="
for %%f in (src\*.c) do set "SRC=!SRC! "%%f""

if not exist build mkdir build
"%CC_BIN%" -std=c23 -O2 -Wall -Wextra -D_CRT_SECURE_NO_WARNINGS -o build\mp3toxwm.exe !SRC!
if errorlevel 1 exit /b 1
echo Built build\mp3toxwm.exe
