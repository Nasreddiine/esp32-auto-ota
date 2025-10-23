@echo off
echo Testing Docker setup...

echo 1. Checking Docker installation...
docker --version
if %errorlevel% neq 0 (
    echo ERROR: Docker not found!
    goto :error
)

echo 2. Testing Docker daemon...
docker ps
if %errorlevel% neq 0 (
    echo ERROR: Docker daemon not running!
    echo Please start Docker Desktop.
    goto :error
)

echo 3. Testing ESP-IDF image...
docker pull espressif/idf:release-v5.1
if %errorlevel% neq 0 (
    echo ERROR: Failed to pull ESP-IDF image!
    goto :error
)

echo.
echo ========================================
echo    DOCKER SETUP TEST: SUCCESS!
echo ========================================
echo You can now run build.bat to build firmware.
pause
exit /b 0

:error
echo.
echo ========================================
echo    DOCKER SETUP TEST: FAILED!
echo ========================================
pause
exit /b 1