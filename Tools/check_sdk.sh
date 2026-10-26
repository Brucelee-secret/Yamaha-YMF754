#!/bin/sh
# Syntax + Mach-O object compilation against the real 10.6 Kernel SDK.
# No linker, kext loading, or hardware access. SDK/objects stay outside Git.
set -eu
if [ "$#" -lt 1 ] || [ "$#" -gt 2 ]; then
    echo "usage: sh Tools/check_sdk.sh /path/MacOSX10.6.sdk [/path/clang++]" >&2
    exit 2
fi
SDK=$(CDPATH= cd -- "$1" && pwd)
CLANGXX=${2:-clang++}
ROOT=$(CDPATH= cd -- "$(dirname "$0")/.." && pwd)
HEADERS="$SDK/System/Library/Frameworks/Kernel.framework/Headers"
if [ ! -f "$HEADERS/IOKit/audio/IOAudioDevice.h" ]; then
    echo "Missing 10.6 Kernel SDK headers: $HEADERS" >&2
    exit 1
fi
RESOURCE=$("$CLANGXX" -print-resource-dir)
TEMP=$(mktemp -d)
trap 'rm -rf "$TEMP"' EXIT HUP INT TERM
"${PYTHON:-python3}" "$ROOT/Tools/embed_firmware.py" \
    "$ROOT/Reference/Firmware/ds1_dsp.fw" \
    "$ROOT/Reference/Firmware/ds1e_ctrl.fw" "$TEMP/YMFPCIFirmware.cpp"
cmp "$TEMP/YMFPCIFirmware.cpp" "$ROOT/Driver/YMFPCIFirmware.cpp"
set -- -target x86_64-apple-darwin10 -mmacosx-version-min=10.6 \
    -isysroot "$SDK" -std=gnu++98 -mkernel -fapple-kext \
    -fno-exceptions -fno-rtti -fno-common -fno-builtin -fno-stack-protector \
    -mno-red-zone -mno-sse -mno-sse2 -mno-mmx -DKERNEL=1 \
    -nostdinc -isystem "$RESOURCE/include" -isystem "$HEADERS" \
    -Wall -Wextra -Werror
"$CLANGXX" --version
for SOURCE in YMFPCIAudioDevice YMFPCIAudioEngine YMFPCIDMA YMFPCIPCM YMFPCIHardware YMFPCIFirmware; do
    "$CLANGXX" "$@" -fsyntax-only "$ROOT/Driver/$SOURCE.cpp"
    "$CLANGXX" "$@" -O2 -c "$ROOT/Driver/$SOURCE.cpp" -o "$TEMP/$SOURCE.o"
    file "$TEMP/$SOURCE.o"
done
echo "PASS: all driver translation units passed SDK 10.6 syntax checks and Mach-O x86_64 object compilation"
echo "NOT CHECKED: LLVM GCC 4.2, Xcode build/link, kext dependencies/load, PCI hardware, audio"
