@echo off
rem Double-click entry for tools/agave.ps1: starts the GUI shell in the
rem process render-backend mode by default (pass -Local to go back to one
rem engine thread, or a URL as the first argument).
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0agave.ps1" %*
if errorlevel 1 pause
