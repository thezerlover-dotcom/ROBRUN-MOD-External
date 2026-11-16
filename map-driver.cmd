@echo off
setlocal
cd /d "%~dp0"
if not exist "RobRunMapper.exe" (
    echo RobRunMapper.exe is missing.
    exit /b 1
)
if not exist "RobRun.sys" (
    echo RobRun.sys is missing.
    exit /b 1
)
"RobRunMapper.exe" "RobRun.sys"
if errorlevel 1 (
    echo Driver mapping failed.
    exit /b 1
)
