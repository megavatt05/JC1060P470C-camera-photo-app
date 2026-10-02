@echo off
setlocal
REM ============================================================
REM  ESP32-P4 (JC1060P470C) - cambrowser firmware 7ab5082
REM  Flash kit prepared from verified CI build (esp32p4-binaries)
REM  Port: COM10   Baud: 921600
REM ============================================================
set PORT=COM10
set BAUD=921600
cd /d "%~dp0"

echo === ESP32-P4: flashing cambrowser (commit 7ab5082) to %PORT% ===
echo.

where esptool >nul 2>nul
if errorlevel 1 (
    echo esptool not found, installing via pip...
    python -m pip install --user esptool
    if errorlevel 1 (
        echo ERROR: could not install esptool. Install Python 3.9+ first.
        pause
        exit /b 1
    )
    set "PATH=%PATH%;%APPDATA%\Python\Python311\Scripts;%APPDATA%\Python\Python312\Scripts;%APPDATA%\Python\Python313\Scripts;%APPDATA%\Python\Scripts"
)

where esptool >nul 2>nul
if errorlevel 1 (
    echo ERROR: esptool installed but not on PATH. Close this window, open a new
    echo console in this folder and run:  esptool --chip esp32p4 --port COM10 --baud 921600 write_flash 0x2000 bins\bootloader\bootloader.bin 0x10000 bins\partition_table\partition-table.bin 0x20000 bins\cambrowser.bin
    pause
    exit /b 1
)

esptool --chip esp32p4 --port %PORT% --baud %BAUD% --before default_reset --after hard_reset write_flash 0x2000 bins\bootloader\bootloader.bin 0x10000 bins\partition_table\partition-table.bin 0x20000 bins\cambrowser.bin

if errorlevel 1 (
    echo.
    echo === FAILED ===
    echo If the board was not detected: hold BOOT, tap RESET, release BOOT, then retry.
    pause
    exit /b 1
)

echo.
echo === SUCCESS: firmware flashed, board is rebooting ===
pause
exit /b 0
