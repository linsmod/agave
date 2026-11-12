@echo off
rem agave.bat - start the agave browser shell in process-render mode.
rem
rem Why a .bat and not a shortcut to the .exe: double-clicking the exe goes
rem through Explorer -> ShellExecute, and Windows SmartScreen may block this
rem unsigned build ("publisher unknown"). cmd starts the exe with CreateProcess,
rem which does not go through SmartScreen, so a double-click on this file works.
rem
rem The `process` backend runs one agave_render_host.exe per tab (S3); if the
rem renderer is missing this falls back to the `local` backend.
rem
rem Usage (all arguments are forwarded):
rem   agave.bat
rem   agave.bat https://chat.deepseek.com/
rem   agave.bat --new-tab https://example.com
rem   agave.bat --new-window --userdata D:\tmp\p2 https://example.com
rem
rem For headless probes use the command line instead (tools\probe.ps1 or
rem agave_shell.exe --probe ...), because `start` detaches the process.

setlocal
cd /d "%~dp0"
set "BUILD=%~dp0build-zig-native"
set "EXE=%BUILD%\agave_shell.exe"
set "RENDERER=%BUILD%\agave_render_host.exe"

if not exist "%EXE%" (
    echo [agave] missing "%EXE%"
    echo [agave] build first: pwsh -File tools\build_project.ps1
    pause
    exit /b 1
)

set "BACKEND=process"
if not exist "%RENDERER%" (
    echo [agave] "%RENDERER%" not found - falling back to the local backend.
    set "BACKEND=local"
)

start "agave" "%EXE%" --backend %BACKEND% %*
exit /b 0
