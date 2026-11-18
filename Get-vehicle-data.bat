@echo off
setlocal enabledelayedexpansion
title OWIS - build vehicle data
echo(
echo   OWIS builds its vehicle data from YOUR OWN licensed diagnostic installation.
echo   It never downloads or ships the manufacturer's data.
echo(
set "PY="
where python >nul 2>nul && set "PY=python"
if not defined PY ( where py >nul 2>nul && set "PY=py" )
if not defined PY (
  echo   Python 3 is required. Install it from https://www.python.org/downloads/ ^(tick "Add to PATH"^),
  echo   then run this file again.
  echo(
  pause & exit /b 1
)
set "SRC=%~1"
if "%SRC%"=="" (
  echo   Drag your installation's "Projects-V" folder onto this window and press Enter,
  echo   e.g.  C:\PIWIS\PIDT\Projects-V       ^(older cars also: ...\PIDT_MIG\Projects-V^)
  echo(
  set /p "SRC=   Projects-V folder: "
)
set "SRC=%SRC:"=%"
set "MIG="
if exist "%SRC%\..\PIDT_MIG\Projects-V" set "MIG=%SRC%\..\PIDT_MIG\Projects-V"
echo(
echo   Building English vehicle data into "%~dp0data" ...
"%PY%" "%~dp0builder\build_data.py" "%SRC%" "%MIG%" --out "%~dp0data"
echo(
echo   Done. Start OWIS.exe - the vehicle list fills up.
pause
