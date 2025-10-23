#!/bin/bash
set -e

echo "Starting ESP32 build process..."

source /opt/esp/idf/export.sh

cd /workspace

if [ ! -f "sdkconfig" ]; then
    echo "Initial project configuration..."
    idf.py set-target esp32
fi

echo "Building firmware..."
idf.py build

echo "Packaging firmware..."
mkdir -p /output
cp build/*.bin /output/ 2>/dev/null || true
cp build/bootloader/bootloader.bin /output/ 2>/dev/null || true
cp build/partition_table/partition-table.bin /output/ 2>/dev/null || true

echo "Generating version info..."
GIT_HASH=$(git rev-parse --short HEAD)
BUILD_DATE=$(date -u +"%Y-%m-%dT%H:%M:%SZ")

cat > /output/version.json << EOF
{
    "version": "${GIT_HASH}",
    "build_date": "${BUILD_DATE}",
    "url": "https://github.com/$GITHUB_REPOSITORY/releases/latest/download/firmware.bin",
    "github_run_id": "${GITHUB_RUN_ID}"
}
EOF

echo "Build complete! Files in /output:"
ls -la /output/