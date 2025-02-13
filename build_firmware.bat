@echo off
setlocal enabledelayedexpansion



REM Ścieżka do platformio.exe
set PIO=C:\Users\johndoe\.platformio\penv\Scripts\platformio.exe

REM Lista środowisk do kompilacji
set ENVIRONMENTS=rak_killer_v2 tbeam t-echo tlora-v2-1-1_6 rak4631 heltec-wireless-tracker heltec-wireless-paper heltec-v3 tbeam-s3-core heltec-mesh-node-t114

REM Katalog wyjściowy
set OUTPUT_DIR=compiled_firmware

REM Tworzenie katalogu wynikowego
if not exist %OUTPUT_DIR% mkdir %OUTPUT_DIR%

REM Kompilacja dla każdego środowiska
for %%E in (%ENVIRONMENTS%) do (
    echo ================================
    echo Building for environment %%E...
    %PIO% run -e %%E 
    if errorlevel 1 (
        echo Error while building %%E
        exit /b 1
    )

    REM Kopiowanie plików wynikowych
    set BUILD_DIR=.pio\build\%%E
    if exist !BUILD_DIR! (
        for /r !BUILD_DIR! %%B in (*.bin) do (
            echo Copying %%B to %OUTPUT_DIR%\%%~nE_%%~nxB
            copy "%%B" "%OUTPUT_DIR%\%%~nE_%%~nxB"
        )
    ) else (
        echo No build directory found for %%E
    )
)

echo ================================
echo Build completed. Files are in %OUTPUT_DIR%.
python firmware_ftp_upload.py
pause