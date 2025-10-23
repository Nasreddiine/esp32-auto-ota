@echo off
echo Cleaning build directories...

if exist build-output (
    rmdir /s /q build-output
    echo Deleted build-output folder
)

if exist build (
    rmdir /s /q build
    echo Deleted build folder
)

echo Cleaning Docker...
docker system prune -f

echo Clean complete!
pause