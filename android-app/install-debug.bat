@echo off
echo ===============================================
echo Building and Installing Nova Emulator to phone...
echo ===============================================
echo.

REM Build and install to phone using gradlew (faster!)
echo [1/3] Building and Installing to phone (R5CWB27N76D)...
echo Running: gradlew installDebug
cd /d "%~dp0"
powershell -Command ".\gradlew.bat installDebug"

if %ERRORLEVEL% NEQ 0 (
    echo ERROR: Build/Install failed!
    pause
    exit /b 1
)

echo Install complete!

REM Launch the app
echo.
echo [2/3] Launching Nova...
adb -s R5CWB27N76D shell am start -n com.nova/.MainActivity

echo.
echo [3/3] Done! Nova should now be open on your phone.
echo.
echo ===============================================
echo DEBUGGING - Nova Logs Only (Clean Format)
echo ===============================================
echo Showing ONLY Nova app logs...
echo Press Ctrl+C to stop
echo.

REM Get Nova PID and show clean logs using PowerShell
powershell -Command "$novaPid = adb -s R5CWB27N76D shell pidof com.nova; if ($novaPid) { adb -s R5CWB27N76D logcat --pid=$novaPid } else { echo 'Could not find Nova PID. Nova may not be running.' }"
