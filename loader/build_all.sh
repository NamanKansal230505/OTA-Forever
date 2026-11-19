#!/usr/bin/env bash
# Builds the loader for every supported chip and collects installer files in $OUT.
# Used by CI; locally:  docker run --rm -v "$PWD:/project" -w /project espressif/idf:v5.4 bash build_all.sh
set -euo pipefail
TARGETS="${TARGETS:-esp32 esp32s2 esp32s3 esp32c3 esp32c6}"
mkdir -p "${OUT:-dist}"
OUT="$(cd "${OUT:-dist}" && pwd)"

for t in $TARGETS; do
  echo "=== $t ==="
  rm -rf build sdkconfig
  idf.py set-target "$t" > /dev/null
  idf.py build
  (
    cd build
    # Single image for manual flashing at 0x0 (overwrites everything)
    esptool.py --chip "$t" merge_bin -o "$OUT/loader-$t-full.bin" @flash_args
  )
  # Separate parts for the web installer: re-installing keeps user firmware and settings
  cp build/bootloader/bootloader.bin              "$OUT/$t-bootloader.bin"
  cp build/partition_table/partition-table.bin    "$OUT/$t-partitions.bin"
  cp build/ota_data_initial.bin                   "$OUT/$t-otadata.bin"
  cp build/otaf_loader.bin                        "$OUT/$t-loader.bin"
done
ls -la "$OUT"
