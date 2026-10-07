@echo off
setlocal EnableExtensions EnableDelayedExpansion

title FrontierMP - Build Release + Log
cd /d "%~dp0"

if not exist "logs" mkdir "logs"

for /f "tokens=1-3 delims=/ " %%a in ("%date%") do (
    set "D1=%%a"
    set "D2=%%b"
    set "D3=%%c"
)
for /f "tokens=1-3 delims=:., " %%a in ("%time%") do (
    set "T1=%%a"
    set "T2=%%b"
    set "T3=%%c"
)

set "STAMP=!D3!-!D2!-!D1!_!T1!-!T2!-!T3!"
set "LOG=logs\build_!STAMP!.log"
set "BUILD_DIR=build"

set "MINHOOK_COMMIT=8af6b4acae5a9388fd742b56fa79ece89d96f823"
set "MINHOOK_URL=https://github.com/TsudaKageyu/minhook/archive/!MINHOOK_COMMIT!.zip"
set "MINHOOK_DIR=vendor\minhook"

echo ========================================================== > "!LOG!"
echo FrontierMP Build Log >> "!LOG!"
echo Started: %date% %time% >> "!LOG!"
echo Project: %cd% >> "!LOG!"
echo ========================================================== >> "!LOG!"

echo.
echo ==========================================================
echo FrontierMP Release Build
echo Project: %cd%
echo Log: !LOG!
echo ==========================================================
echo.

rem ----------------------------------------------------------
rem 1. Prepare MinHook
rem GitHub ZIP downloads do not include initialized submodules.
rem We download the exact revision referenced by this project.
rem ----------------------------------------------------------
echo [1/3] Checking MinHook dependency...
echo [1/3] Checking MinHook dependency... >> "!LOG!"

if not exist "!MINHOOK_DIR!\src\buffer.c" (
    echo [INFO] MinHook is missing. Downloading pinned revision...
    echo [INFO] MinHook is missing. Downloading pinned revision... >> "!LOG!"

    set "MINHOOK_TMP=!TEMP!\FrontierMP_Minhook_!RANDOM!"
    mkdir "!MINHOOK_TMP!" >> "!LOG!" 2>&1

    powershell -NoProfile -ExecutionPolicy Bypass -Command ^
      "$ErrorActionPreference='Stop'; Invoke-WebRequest -UseBasicParsing -Uri '!MINHOOK_URL!' -OutFile '!MINHOOK_TMP!\minhook.zip'; Expand-Archive -Force '!MINHOOK_TMP!\minhook.zip' '!MINHOOK_TMP!\extract'; New-Item -ItemType Directory -Force -Path '%CD%\vendor' | Out-Null; if(Test-Path '%CD%\vendor\minhook'){Remove-Item '%CD%\vendor\minhook' -Recurse -Force}; Move-Item '!MINHOOK_TMP!\extract\minhook-!MINHOOK_COMMIT!' '%CD%\vendor\minhook';" >> "!LOG!" 2>&1

    if not "!ERRORLEVEL!"=="0" (
        echo.
        echo [ERROR] Failed to download MinHook.
        echo [ERROR] Failed to download MinHook. >> "!LOG!"
        echo Check your internet connection and try again.
        goto BUILD_ERROR
    )

    rmdir /s /q "!MINHOOK_TMP!" >> "!LOG!" 2>&1
)

if not exist "!MINHOOK_DIR!\src\buffer.c" (
    echo.
    echo [ERROR] MinHook is still missing:
    echo         !MINHOOK_DIR!\src\buffer.c
    echo [ERROR] MinHook is still missing: !MINHOOK_DIR!\src\buffer.c >> "!LOG!"
    goto BUILD_ERROR
)

echo [OK] MinHook dependency is ready.
echo [OK] MinHook dependency is ready. >> "!LOG!"
echo.

rem ----------------------------------------------------------
rem 2. Clean CMake configure directory
rem This prevents CMakeCache.txt from another absolute path from
rem being reused when the project is copied/moved.
rem ----------------------------------------------------------
echo [2/3] Preparing clean CMake build directory...
echo [2/3] Preparing clean CMake build directory... >> "!LOG!"

if exist "!BUILD_DIR!" (
    echo [INFO] Removing !BUILD_DIR! to avoid stale CMake cache...
    echo [INFO] Removing !BUILD_DIR! to avoid stale CMake cache... >> "!LOG!"
    rmdir /s /q "!BUILD_DIR!" >> "!LOG!" 2>&1
)

mkdir "!BUILD_DIR!" >> "!LOG!" 2>&1

echo.
echo [INFO] Configuring CMake for x64...
echo cmake -S . -B "!BUILD_DIR!" -A x64 >> "!LOG!"

cmake -S . -B "!BUILD_DIR!" -A x64 >> "!LOG!" 2>&1
set "CFG_EXIT=!ERRORLEVEL!"

if not "!CFG_EXIT!"=="0" (
    echo.
    echo [ERROR] CMake configuration failed with code !CFG_EXIT!.
    goto BUILD_ERROR
)

echo [OK] CMake configuration completed.
echo [OK] CMake configuration completed. >> "!LOG!"
echo.

rem ----------------------------------------------------------
rem 3. Build Release
rem ----------------------------------------------------------
echo [3/3] Building Release...
echo [3/3] Building Release... >> "!LOG!"
echo cmake --build "!BUILD_DIR!" --config Release --parallel >> "!LOG!"

cmake --build "!BUILD_DIR!" --config Release --parallel >> "!LOG!" 2>&1
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

:BUILD_ERROR
echo. >> "!LOG!"
echo ========================================================== >> "!LOG!"
echo BUILD PREPARATION/CONFIGURATION FAILED >> "!LOG!"
echo Finished: %date% %time% >> "!LOG!"
echo ========================================================== >> "!LOG!"

echo.
echo ---------------- LOG ----------------
type "!LOG!"
echo -------------- END LOG --------------
echo.
echo The window will remain open.
pause
exit /b 20
