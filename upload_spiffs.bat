@echo off
setlocal
set PORT=%1
if "%PORT%"=="" set PORT=COM3
if not defined IDF_PATH ( echo ERROR: Run from ESP-IDF 5.5 CMD & pause & exit /b 1 )
python %IDF_PATH%\components\esptool_py\esptool\esptool.py --chip esp32s3 --port %PORT% write_flash 0x1B0000 data\frames.bin
echo Done.
endlocal
