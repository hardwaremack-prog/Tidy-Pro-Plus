#!/usr/bin/env bash
# TidyOS Pro - builds everything for the Heltec V3, V4, V4-R8 and the Seeed XIAO ESP32S3, and packages it for the web flasher.
# The V3 (8 MB flash, no PSRAM) and the XIAO (8 MB flash, 8 MB PSRAM) get TidyOS Pro only (no Shelf), with
# source/TidyOSPro/partitions-v3.csv (XIAO: partitions-xiao.csv, a bigger app slot). The XIAO build (-DTIDYOS_XIAO)
# adds a VGA monitor output (vga_s3.cpp) and Bluetooth keyboard + mouse.
# ONLY=v3 (or "v4 r8") builds just those boards. SKIP_SHELF=1 reuses The Shelf from the images already in firmware/.
#
#   TidyOSPro   TidyOS Pro itself                    -> the "tidyos" app slot
#   TidyBoot    the boot menu (and update installer)  -> the "menu" (factory) slot
#   ShelfDual   The Shelf, built for dual boot        -> the "shelf" app slot (+ its web app in "www")
#
# Needs: arduino-cli, esp32 core 2.0.17 (FabGL doesn't build on 3.x), and the libraries:
#   libraries/FabGL      FabGL 1.0.9 with TidyOS's patches and the ESP32-S3 port (in this repo)
#   RadioLib 7.1.2, U8g2, NimBLE-Arduino 1.4.3, ESPAsyncWebServer 3.3.23 + ArduinoJson 7 (for The Shelf)
#   source/ShelfDual/libraries (The Shelf's own SD + AsyncTCP)
# The Shelf's web app image comes ready-made from The Shelf's installer site (SHELF_WWW=... to override).
set -euo pipefail
cd "$(dirname "$0")"
VER=$(sed -n 's/^#define TIDYOS_VERSION "\(.*\)"/\1/p' source/TidyOSPro/config.h)
B=${BUILD:-build}; mkdir -p "$B"
CORE="$HOME/Arduino/hardware/espressif/esp32"
TP=()
if [ -d "$CORE/tools/xtensa-esp32s3-elf" ]; then      # core installed by hand rather than Boards Manager
  TP=(--build-property "runtime.tools.esptool_py.path=$CORE/tools/esptool"
      --build-property "runtime.tools.xtensa-esp32s3-elf-gcc.path=$CORE/tools/xtensa-esp32s3-elf"
      --build-property "runtime.tools.xtensa-esp32-elf-gcc.path=$CORE/tools/xtensa-esp32-elf")
  command -v ctags >/dev/null && TP+=(--build-property "runtime.tools.ctags.path=$(dirname "$(command -v ctags)")")
fi
cp source/TidyOSPro/partitions.csv source/TidyBoot/partitions.csv
cp source/TidyOSPro/partitions.csv source/ShelfDual/ShelfV4/partitions.csv

for v in ${ONLY:-v4 r8 v3 xiao}; do
  psram=enabled; sfx=""; flash=16M; xf=""; SK=source/TidyOSPro; BK=source/TidyBoot; usb="USBMode=hwcdc,CDCOnBoot=cdc"; maxapp=3145728
  [ $v = r8 ] && psram=opi && sfx=r8
  if [ $v = v3 ]; then                       # Heltec V3: 8 MB flash, no PSRAM, its own partition table
    # its USB-C goes through a CP2102 to UART0 (GPIO 43/44), not to the S3's own USB: Serial must be UART0,
    # or the console, Files over USB and the flasher's board checks hear nothing
    psram=disabled; sfx=v3; flash=8M; xf="-DTIDYOS_V3"; usb="USBMode=hwcdc,CDCOnBoot=default"
    rm -rf "$B/src-v3"; mkdir -p "$B/src-v3"; cp -a source/TidyOSPro source/TidyBoot "$B/src-v3/"
    cp source/TidyOSPro/partitions-v3.csv "$B/src-v3/TidyOSPro/partitions.csv"
    cp source/TidyOSPro/partitions-v3.csv "$B/src-v3/TidyBoot/partitions.csv"
    SK="$B/src-v3/TidyOSPro"; BK="$B/src-v3/TidyBoot"
  fi
  if [ $v = xiao ]; then                     # Seeed XIAO ESP32S3 + Wio-SX1262: 8 MB flash, 8 MB octal PSRAM, native USB,
    psram=opi; sfx=xiao; flash=8M; xf="-DTIDYOS_XIAO"; maxapp=3670016   # partitions-xiao.csv: a 3.5 MB TidyOS Pro slot
    rm -rf "$B/src-xiao"; mkdir -p "$B/src-xiao"; cp -a source/TidyOSPro source/TidyBoot "$B/src-xiao/"
    cp source/TidyOSPro/partitions-xiao.csv "$B/src-xiao/TidyOSPro/partitions.csv"
    cp source/TidyOSPro/partitions-xiao.csv "$B/src-xiao/TidyBoot/partitions.csv"
    SK="$B/src-xiao/TidyOSPro"; BK="$B/src-xiao/TidyBoot"
  fi
  F="espressif:esp32:esp32s3:FlashSize=$flash,FlashMode=qio,PSRAM=$psram,$usb,CPUFreq=240"
  arduino-cli compile --fqbn "$F" ${TP[@]+"${TP[@]}"} --library "$PWD/libraries/FabGL" \
    --build-property "compiler.cpp.extra_flags=-DTIDYOS_HTML5 $xf" \
    --build-property "compiler.c.elf.extra_flags=-Wl,--wrap=ff_disk_read -Wl,--wrap=ff_disk_write -Wl,--wrap=esp_panic_handler" \
    --build-property "upload.maximum_size=$maxapp" --build-path "$B/b-s3$sfx" "$SK"
  # the full build: every TidyOS-ESP32 app as well (the flasher picks which ones show)
  arduino-cli compile --fqbn "$F" ${TP[@]+"${TP[@]}"} --library "$PWD/libraries/FabGL" \
    --build-property "compiler.cpp.extra_flags=-DTIDYOS_HTML5 -DTIDY_ALL_APPS=1 $xf" \
    --build-property "compiler.c.elf.extra_flags=-Wl,--wrap=ff_disk_read -Wl,--wrap=ff_disk_write -Wl,--wrap=esp_panic_handler" \
    --build-property "upload.maximum_size=$maxapp" --build-path "$B/b-s3${sfx}full" "$SK"
  arduino-cli compile --fqbn "$F" ${TP[@]+"${TP[@]}"} --build-property "compiler.cpp.extra_flags=$xf" --build-property "upload.maximum_size=458752" --build-path "$B/b-boot$sfx" "$BK"
  [ $v = v3 ] && continue                    # no Shelf on the V3
  [ $v = xiao ] && continue                  # ...or on the XIAO (8 MB too)
  [ -n "${SKIP_SHELF:-}" ] && continue
  arduino-cli compile --fqbn "$F" ${TP[@]+"${TP[@]}"} \
    --build-property "compiler.cpp.extra_flags=-DCONFIG_ASYNC_TCP_RUNNING_CORE=0 -DCONFIG_ASYNC_TCP_STACK_SIZE=16384 -DCONFIG_ASYNC_TCP_QUEUE_SIZE=256 -DSHELF_DUALBOOT" \
    --build-property "compiler.c.elf.extra_flags=-Wl,--wrap=esp_panic_handler" \
    --build-property "upload.maximum_size=2097152" --library "$PWD/source/ShelfDual/libraries/SD" --library "$PWD/source/ShelfDual/libraries/AsyncTCP" \
    --build-path "$B/b-shelf$sfx" source/ShelfDual/ShelfV4
done
rm -f source/TidyBoot/partitions.csv source/ShelfDual/ShelfV4/partitions.csv
# The Shelf's web app (LittleFS image), as published by The Shelf's installer site
S=https://hardwaremack-prog.github.io/The-Shelf-Heltec-V4900mhzMesh/ota
[ -f "$B/shelf-v4-www.bin.gz" ]    || curl -fsSL -o "$B/shelf-v4-www.bin.gz"    "${SHELF_WWW:-$S/shelf-v4-www.bin.gz}"
[ -f "$B/shelf-v4-r8-www.bin.gz" ] || curl -fsSL -o "$B/shelf-v4-r8-www.bin.gz" "${SHELF_WWW_R8:-$S/shelf-v4-r8-www.bin.gz}"
python3 tools/package.py "$VER" "$B" "$B/shelf-v4-www.bin.gz" "$B/shelf-v4-r8-www.bin.gz"
python3 tools/parts.py
ls -la firmware
