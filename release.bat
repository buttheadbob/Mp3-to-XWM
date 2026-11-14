@echo off
rem SPDX-License-Identifier: AGPL-3.0-or-later
rem Builds mp3toxwm.exe and packs it with README.md, LICENSE and THIRD_PARTY.md into
rem release\mp3toxwm-<version>-windows-x64.zip, with a .sha256 file beside it.
rem The version comes from src\version.h. Uses only Windows' own PowerShell for the zip and hash.
setlocal
cd /d "%~dp0"

call "%~dp0build.bat" || exit /b 1

set "VERSION="
for /f "tokens=3" %%v in ('findstr /b /c:"#define MP3TOXWM_VERSION " src\version.h') do set "VERSION=%%~v"
if not defined VERSION (
    echo No MP3TOXWM_VERSION in src\version.h.
    exit /b 1
)

set "NAME=mp3toxwm-%VERSION%-windows-x64"
set "STAGE=release\%NAME%"
if exist "%STAGE%" rmdir /s /q "%STAGE%"
if exist "release\%NAME%.zip" del "release\%NAME%.zip"
mkdir "%STAGE%" || exit /b 1
copy /y build\mp3toxwm.exe "%STAGE%\" >nul || exit /b 1
copy /y README.md "%STAGE%\" >nul || exit /b 1
copy /y LICENSE "%STAGE%\" >nul || exit /b 1
copy /y THIRD_PARTY.md "%STAGE%\" >nul || exit /b 1

powershell -NoProfile -Command ^
    "$ErrorActionPreference = 'Stop';" ^
    "Compress-Archive -Path 'release\%NAME%\*' -DestinationPath 'release\%NAME%.zip';" ^
    "$s = [IO.File]::OpenRead('release\%NAME%.zip');" ^
    "$h = -join ([Security.Cryptography.SHA256]::Create().ComputeHash($s) | ForEach-Object { $_.ToString('x2') }); $s.Close();" ^
    "[IO.File]::WriteAllText('release\%NAME%.zip.sha256', $h + '  %NAME%.zip' + [char]10)" || exit /b 1
rmdir /s /q "%STAGE%"
echo Built release\%NAME%.zip
