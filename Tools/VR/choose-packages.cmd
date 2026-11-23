@echo off
setlocal
if exist "%~dp0urSovngardeSE.exe" (
    "%~dp0urSovngardeSE.exe" --packages
) else (
    "%~dp0urSovngarde.exe" --packages
)
exit /b %errorlevel%
