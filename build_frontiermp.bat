@echo off
setlocal EnableExtensions EnableDelayedExpansion

title FrontierMP - Build Release + Log

cd /d "%~dp0"

if not exist "logs" mkdir "logs"

rem Use a clean out-of-source build directory so a copied repository never reuses
rem a CMakeCache generated in another absolute path.
set "BUILD_DIR=build_release"

set "MINHOOK_COMMIT=8af6b4acae5a9388fd742b56fa79ece89d96f823"
set "MINHOOK_URL=https://github.com/TsudaKageyu/minhook/archive/!MINHOOK_COMMIT!.zip"

rem GitHub ZIP downloads do not contain initialized submodules. Bootstrap MinHook locally.
if not exist "vendor\minhook\src\hook.c" (
    echo.
    echo [INFO] MinHook sources are missing. Downloading the pinned dependency...
    echo [INFO] MinHook sources are missing. Downloading the pinned dependency... >> "!LOG!"
    set "MINHOOK_TMP=!TEMP!\FrontierMP_Minhook_!RANDOM!"
    mkdir "!MINHOOK_TMP!" >> "!LOG!" 2>&1

    powershell -NoProfile -ExecutionPolicy Bypass -Command ^
      "$ErrorActionPreference='Stop'; Invoke-WebRequest -UseBasicParsing -Uri '!MINHOOK_URL!' -OutFile '!MINHOOK_TMP!\minhook.zip'; Expand-Archive -Force '!MINHOOK_TMP!\minhook.zip' '!MINHOOK_TMP!\extract'; New-Item -ItemType Directory -Force -Path '%CD%\vendor' | Out-Null; Move-Item '!MINHOOK_TMP!\extract\minhook-!MINHOOK_COMMIT!' '%CD%\vendor\minhook';" >> "!LOG!" 2>&1

    if not "!ERRORLEVEL!"=="0" (
        echo [ERROR] Could not download/bootstrap MinHook.
        echo [ERROR] Could not download/bootstrap MinHook. >> "!LOG!"
        echo Check internet access or clone the repository with submodules.
        echo.
        type "!LOG!"
        echo.
        pause
        exit /b 20
    )

    rmdir /s /q "!MINHOOK_TMP!" >> "!LOG!" 2>&1
    echo [OK] MinHook dependency is ready.
    echo [OK] MinHook dependency is ready. >> "!LOG!"
)

rem ----------------------------------------------------------
rem Prepare MinHook. GitHub source archives do not include the
rem contents of the MinHook submodule, only the submodule entry.
rem ----------------------------------------------------------
set "MINHOOK_DIR=vendor\minhook"
if not exist "!MINHOOK_DIR!\src\buffer.c" (
    echo.
    echo [INFO] MinHook sources are missing. Preparing vendor\minhook...
    echo [INFO] MinHook sources are missing. Preparing vendor\minhook... >> "!LOG!"

    where git >nul 2>&1
    if "!ERRORLEVEL!"=="0" (
        echo [INFO] Git found. Cloning MinHook...
        echo [INFO] Git found. Cloning MinHook... >> "!LOG!"
        if exist "!MINHOOK_DIR!" rmdir /s /q "!MINHOOK_DIR!" >> "!LOG!" 2>&1
        git clone --depth 1 https://github.com/TsudaKageyu/minhook.git "!MINHOOK_DIR!" >> "!LOG!" 2>&1
        if not "!ERRORLEVEL!"=="0" (
            echo [ERROR] Git could not clone MinHook.
            echo [ERROR] Git could not clone MinHook. >> "!LOG!"
            goto BUILD_ERROR
        )
    ) else (
        echo [INFO] Git not found. Downloading MinHook ZIP with PowerShell...
        echo [INFO] Git not found. Downloading MinHook ZIP with PowerShell... >> "!LOG!"
        powershell -NoProfile -ExecutionPolicy Bypass -Command ^
          "$ErrorActionPreference='Stop'; $zip=Join-Path $env:TEMP 'minhook-master.zip'; $root=Join-Path $env:TEMP 'minhook-master'; if(Test-Path $zip){Remove-Item $zip -Force}; if(Test-Path $root){Remove-Item $root -Recurse -Force}; Invoke-WebRequest -UseBasicParsing 'https://github.com/TsudaKageyu/minhook/archive/refs/heads/master.zip' -OutFile $zip; Expand-Archive -Path $zip -DestinationPath $env:TEMP -Force; New-Item -ItemType Directory -Force -Path 'vendor' | Out-Null; if(Test-Path 'vendor\minhook'){Remove-Item 'vendor\minhook' -Recurse -Force}; Move-Item $root 'vendor\minhook'" >> "!LOG!" 2>&1
        if not "!ERRORLEVEL!"=="0" (
            echo [ERROR] PowerShell could not download MinHook.
            echo [ERROR] PowerShell could not download MinHook. >> "!LOG!"
            goto BUILD_ERROR
        )
    )
)

if not exist "!MINHOOK_DIR!\src\buffer.c" (
    echo [ERROR] MinHook is still missing: !MINHOOK_DIR!\src\buffer.c
    echo [ERROR] MinHook is still missing: !MINHOOK_DIR!\src\buffer.c >> "!LOG!"
    goto BUILD_ERROR
)

echo [OK] MinHook sources are ready.
echo [OK] MinHook sources are ready. >> "!LOG!"

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

echo [1/2] Preparing clean CMake build directory...
echo [1/2] Preparing clean CMake build directory... >> "!LOG!"
echo.

if exist "!BUILD_DIR!\CMakeCache.txt" (
    echo [INFO] Existing CMake cache found in !BUILD_DIR! - removing it... 
    echo [INFO] Existing CMake cache found in !BUILD_DIR! - removing it... >> "!LOG!"
    rmdir /s /q "!BUILD_DIR!" >> "!LOG!" 2>&1
)

if exist "!BUILD_DIR!" (
    echo [INFO] Reusing directory !BUILD_DIR! after cache cleanup.
) else (
    mkdir "!BUILD_DIR!" >> "!LOG!" 2>&1
)

echo [1/2] Configuring CMake...
echo [1/2] Configuring CMake... >> "!LOG!"

echo cmake -S . -B !BUILD_DIR! -A x64 >> "!LOG!"
cmake -S . -B "!BUILD_DIR!" -A x64 >> "!LOG!" 2>&1
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

echo cmake --build !BUILD_DIR! --config Release --parallel >> "!LOG!"
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


:BUILD_ERROR
echo.
echo ==========================================================
echo [FAILED] Build preparation failed.
echo [LOG] !LOG!
echo ==========================================================
echo.
echo ---------------- LOG ----------------
type "!LOG!"
echo -------------- END LOG --------------
echo.
echo The window will remain open.
pause
exit /b 1

exit /b !BUILD_EXIT!
