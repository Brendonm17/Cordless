@echo off
REM Double-click to run set-token.ps1 with the execution policy bypassed.
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0set-token.ps1" %*
echo.
pause
