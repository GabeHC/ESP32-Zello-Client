@echo off
set ESP32_IP=192.168.1.xxx
echo Building and uploading to %ESP32_IP%...
pio run -t upload
if errorlevel 1 (
    echo Upload failed!
    pause
    exit /b 1
)
echo Upload successful!
echo Opening debug page...
start http://%ESP32_IP%/debug
pause