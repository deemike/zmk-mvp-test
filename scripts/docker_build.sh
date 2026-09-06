#!/bin/bash
set -e

if [ ! -d "/work/zmk/app" ]; then
    echo "=== Initializing ZMK workspace ==="
    cd /work
    rm -rf .west
    west init -m https://github.com/zmkfirmware/zmk --mr main --mf app/west.yml .
    west update
fi

echo "=== Exporting Zephyr package ==="
cd /work
west zephyr-export

echo "=== Building ZMK Firmware ==="
west build -p -b xiao_ble//zmk -s /work/zmk/app -d /work/build -- -DSHIELD=fido_scanner -DZMK_CONFIG=/config/config

if [ -f "/work/build/zephyr/zmk.uf2" ]; then
    cp /work/build/zephyr/zmk.uf2 /config/zmk.uf2
    echo "SUCCESS: zmk.uf2 generated in workspace root"
else
    echo "ERROR: zmk.uf2 was not found"
    exit 1
fi
