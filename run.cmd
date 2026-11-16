@echo off
setlocal
cd /d "%~dp0"
if not exist "RobRun.exe" (
    echo RobRun.exe is missing.
    exit /b 1
)
start "" "RobRun.exe"
