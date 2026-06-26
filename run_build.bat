@echo off
REM T-Display-S3 GIF Player Build
setlocal
set PORT=%1
if "%PORT%"=="" set PORT=COM3
echo === Build ===
if not defined IDF_PATH ( echo ERROR: Run from ESP-IDF 5.5 CMD & pause & exit /b 1 )
idf.py build
if %ERRORLEVEL% neq 0 ( echo BUILD FAILED & pause & exit /b 1 )
echo.
echo Flashing...
idf.py -p %PORT% flash
if %ERRORLEVEL% neq 0 ( echo FLASH FAILED & pause & exit /b 1 )
echo.
echo Uploading SPIFFS...
python %IDF_PATH%\components\esptool_py\esptool\esptool.py --chip esp32s3 --port %PORT% write_flash 0x1B0000 data\frames.bin
echo.
echo Monitor (Ctrl+] to exit)...
idf.py -p %PORT% monitor
endlocal
