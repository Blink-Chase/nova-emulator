@echo off
echo ===============================================
echo Nova Emulator Debug Log
echo ===============================================
echo.
echo Launching App...
adb -s R5CWB27N76D shell am start -n com.nova/.MainActivity

echo.
echo Monitoring Logcat (Press Ctrl+C to stop)...
adb -s R5CWB27N76D logcat *:E Nova:D
