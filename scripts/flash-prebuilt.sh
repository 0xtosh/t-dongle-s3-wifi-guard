#!/bin/bash

BASE_PATH="../firmware"
PORT=$1

show_help() {
    echo "LilyGO T-Dongle-S3 Flashing Utility"
    echo "-----------------------------------"
    echo "Usage: $0 [PORT]"
    echo ""
    echo "Arguments:"
    echo "  PORT          The device path (e.g., /dev/cu.usbmodemxxxx) Run ls /dev/cu.* to find your device"
    echo ""
    echo "Options:"
    echo "  -h, --help    Show this help message"
    echo ""
    echo "Required Files in $BASE_PATH/:"
    echo "  - bootloader.bin (at 0x0000)"
    echo "  - partitions.bin (at 0x8000)"
    echo "  - firmware.bin   (at 0x10000)"
}

if [[ "$PORT" == "-h" || "$PORT" == "--help" ]]; then
    show_help
    exit 0
fi

if ! command -v esptool.py &> /dev/null; then
    echo "❌ Error: esptool.py is not installed or not in your PATH."
    echo "👉 Install it with: pip install esptool"
    exit 1
fi

if [ -z "$PORT" ]; then
    echo "❌ Error: No port specified."
    show_help
    echo ""
    echo "-----------------------------------"
    echo ""
    echo "Running 'ls /dev/cu.*' for you, use one of these as your port"
    echo ""
    ls /dev/cu.*

    exit 1
fi

for FILE in "bootloader.bin" "partitions.bin" "firmware.bin"; do
    if [ ! -f "$BASE_PATH/$FILE" ]; then
        echo "❌ Error: $BASE_PATH/$FILE not found!"
        exit 1
    fi
done

echo "🚀 Initializing LilyGO T-Dongle-S3 flash..."
echo "📍 Port: $PORT"

esptool.py --chip esp32-s3 --port "$PORT" --baud 921600 write_flash -z \
    0x0000 "$BASE_PATH/bootloader.bin" \
    0x8000 "$BASE_PATH/partitions.bin" \
    0x10000 "$BASE_PATH/firmware.bin"

if [ $? -eq 0 ]; then
    echo "✅ Flash successful!"
else
    echo "❌ Flash failed. Check your connection or Boot mode."
fi