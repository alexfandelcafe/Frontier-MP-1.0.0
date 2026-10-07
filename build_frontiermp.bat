@echo off
setlocal EnableExtensions EnableDelayedExpansion

title FrontierMP - Build Release + Log

cd /d "%~dp0"

if not exist "logs" mkdir "logs"

for /f "tokens=1-3 delims=/ " %%a in ('echo %date%') do (
    set "D1=%%a"
    set "D2=%%b"
    set "D3=%%c"
)
for /f "tokens=1-3 delims=:., " %%a in ('echo %time%') do (
    set "T1=%%a"
    set "T2=%%b"
    set "T3=%%c"
)

set "STAMP=!D3!-!D2!-!D1!_!T1!-!T2!-!T3!"
set "LOG=logs\build_!STAMP!.log"

echo ========================================================== > "!LOG!"
echo FrontierMP Build Log >> "!LOG!"
echo Started: %date% %time% >> "!LOG!"
echo Project: %cd% >> "!LOG!"
echo ========================================================== >> "!LOG!"
echo.

echo [1/2] Configuring CMake...
echo [1/2] Configuring CMake...
echo.

cmake -S . -B build -A x64 >> "!LOG!" 2>&1
set "CFG_EXIT=!ERRORLEVEL!"

if not "!CFG_EXIT!"=="0" (
    echo.
    echo [ERROR] CMake configuration failed with code !CFG_EXIT!.
    echo [ERROR] See "!LOG!".
    echo.
    echo ---------------- LOG ----------------
    type "!LOG!"
    echo -------------- END LOG --------------
    echo.
    pause
    exit /b !CFG_EXIT!
)

echo [OK] CMake configuration completed.
echo [OK] CMake configuration completed. >> "!LOG!"
echo.

echo [2/2] Building Release...
echo [2/2] Building Release...
echo.

cmake --build build --config Release --parallel >> "!LOG!" 2>&1
set "BUILD_EXIT=!ERRORLEVEL!"

echo. >> "!LOG!"
echo ========================================================== >> "!LOG!"
echo Finished: %date% %time% >> "!LOG!"
echo Build exit code: !BUILD_EXIT! >> "!LOG!"
echo ========================================================== >> "!LOG!"

if "!BUILD_EXIT!"=="0" (
    echo.
    echo ==========================================================
    echo [SUCCESS] FrontierMP Release build completed.
    echo [LOG] !LOG!
    echo ==========================================================
) else (
    echo.
    echo ==========================================================
    echo [FAILED] FrontierMP Release build failed.
    echo [CODE] !BUILD_EXIT!
    echo [LOG] !LOG!
    echo ==========================================================
)

echo.
echo ---------------- LOG ----------------
type "!LOG!"
echo -------------- END LOG --------------
echo.
echo The window will remain open.
pause

exit /b !BUILD_EXIT!
