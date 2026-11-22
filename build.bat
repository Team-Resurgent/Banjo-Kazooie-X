@echo off
rem Banjo-Kazooie-X: one double-click from your ROM to Banjo-Kazooie-X.iso.
rem
rem   1. put your Banjo-Kazooie ROM in this folder, named baserom.z64
rem   2. run this file (double-click, or "build.bat" in a console)
rem
rem It finds a Python 3.8 or later and hands over to setup.py, which checks the
rem ROM, fetches Torch and builds it, extracts the game from the ROM and
rem builds. Arguments are
rem passed on: build.bat --check, build.bat --skip-build, build.bat --hd-packs.
rem Run with none, it asks whether to download the HD texture packs too.
setlocal EnableExtensions
set "BKX_SELF=%~f0"
if not defined PATH set "PATH=%SystemRoot%\System32;%SystemRoot%;%SystemRoot%\System32\Wbem"
cd /d "%~dp0"
title Banjo-Kazooie-X build

echo.
echo  Banjo-Kazooie-X -- Banjo-Kazooie (Lighthouse) for the original Xbox
echo.

set "BKX_ROM="
rem --check reports a missing ROM itself, with everything else
if /i "%~1"=="--check" set "BKX_ROM=check"
for %%R in (baserom.z64 baserom.v64 baserom.n64 bk.z64 bk.v64 bk.n64) do (
  if not defined BKX_ROM if exist "%%R" set "BKX_ROM=%%R"
)
if not defined BKX_ROM (
  echo  Your ROM is not here. Copy your Banjo-Kazooie ROM into
  echo      %~dp0
  echo  and name it  baserom.z64  then run this again.
  goto :fail
)

set "BKX_PY="
for %%V in (3.13 3.12 3.14 3.15 3.11 3.10 3.9 3.8) do (
  if not defined BKX_PY (
    py -%%V -c "import sys" >nul 2>nul && set "BKX_PY=py -%%V"
  )
)
if not defined BKX_PY (
  python -c "import sys; sys.exit(0 if sys.version_info >= (3, 8) else 1)" >nul 2>nul && set "BKX_PY=python"
)
if not defined BKX_PY (
  echo  No Python 3.8 or later was found. Install it from
  echo      https://www.python.org/downloads/
  echo  ^(tick "Add python.exe to PATH"^), then run this again.
  goto :fail
)

echo  Python: %BKX_PY%
echo.

rem the HD texture packs: asked once, on a double-click with none in packs\ yet (setup.py --hd-packs)
set "BKX_PACKS="
if "%~1"=="" if not defined BKX_NONINTERACTIVE if not exist "packs\*.o2r" (
  echo  HD texture packs: BKHD Loulof ^(the whole game^) and BK Reloaded ^(the interface^)
  echo  can be downloaded ^(about 490 MB^) and put on the disc. The game starts without
  echo  them: turn them on in the game ^(Back, Mods, and Options, HD textures^).
  choice /c YN /m " Download them"
  if not errorlevel 2 set "BKX_PACKS=--hd-packs"
  echo.
)
%BKX_PY% setup.py %BKX_PACKS% %*
set "BKX_RC=%ERRORLEVEL%"
if not "%BKX_RC%"=="0" goto :failrc
echo.
echo  Done.
call :pause
exit /b 0

:failrc
echo.
echo  The build stopped (exit code %BKX_RC%). The messages above say why.
call :pause
exit /b %BKX_RC%

:fail
echo.
call :pause
exit /b 1

rem ---- keep a double-clicked window open --------------------------------
:pause
if defined BKX_NONINTERACTIVE exit /b 0
echo %cmdcmdline% | find /i "%BKX_SELF%" >nul
if not errorlevel 1 pause
exit /b 0
