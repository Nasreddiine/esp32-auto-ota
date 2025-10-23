@echo off
echo ========================================
echo    ESP32 Auto-OTA Docker Builder
echo ========================================

echo Checking if Docker is running...
docker version >nul 2>&1
if %errorlevel% neq 0 (
    echo ERROR: Docker is not running!
    echo Please start Docker Desktop and try again.
    pause
    exit /b 1
)

echo Building Docker image...
docker build -t esp32-auto-ota .

if %errorlevel% neq 0 (
    echo ERROR: Docker build failed!
    pause
    exit /b 1
)

echo Creating build-output directory...
if not exist build-output mkdir build-output

echo Running firmware build...
docker run --rm ^
  -v "%cd%:/workspace" ^
  -v "%cd%/build-output:/output" ^
  -e GITHUB_REPOSITORY=yourusername/esp32-auto-ota ^
  esp32-auto-ota

if %errorlevel% neq 0 (
    echo ERROR: Docker run failed!
    pause
    exit /b 1
)

echo.
echo ========================================
echo         BUILD COMPLETE!
echo ========================================
echo Generated files in build-output folder:
dir build-output

echo.
echo Next steps:
echo 1. Check build-output folder for .bin files
echo 2. Flash firmware.bin to your ESP32
echo 3. Push code to GitHub to trigger auto-OTA
echo.
pause