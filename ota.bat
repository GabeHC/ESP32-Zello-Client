@echo off
set /p ESP32_IP="Enter ESP32 IP: "
echo Testing %ESP32_IP%...
ping -n 1 %ESP32_IP%
if errorlevel 1 (
    echo Cannot reach ESP32
    pause
    exit /b 1
)
echo Uploading via OTA to %ESP32_IP%...
pio run -t upload --upload-port %ESP32_IP%
if errorlevel 1 (
    echo Upload failed!
    echo Make sure:
    echo - ESP32 is powered on
    echo - IP address is correct
    echo - Web update is enabled in firmware
) else (
    echo Upload successful!
    start http://%ESP32_IP%/debug
)
pause