#!/bin/sh
# Portable sequence tests; NOT a substitute for an Xcode build or hardware test.
set -eu
ROOT=$(CDPATH= cd -- "$(dirname "$0")/.." && pwd)
TEMP=$(mktemp -d)
trap 'rm -rf "$TEMP"' EXIT HUP INT TERM
PYTHON=${PYTHON:-python3}
CXX=${CXX:-c++}
"$PYTHON" "$ROOT/Tools/embed_firmware.py" \
    "$ROOT/Reference/Firmware/ds1_dsp.fw" \
    "$ROOT/Reference/Firmware/ds1e_ctrl.fw" "$TEMP/YMFPCIFirmware.cpp"
cmp "$TEMP/YMFPCIFirmware.cpp" "$ROOT/Driver/YMFPCIFirmware.cpp"
"$CXX" -std=c++98 -Wall -Wextra -Werror -pedantic \
    "$ROOT/Driver/YMFPCIHardware.cpp" \
    "$ROOT/Driver/YMFPCIFirmware.cpp" "$ROOT/Tests/hardware_test.cpp" \
    -o "$TEMP/hardware_test"
"$TEMP/hardware_test"
"$CXX" -std=c++98 -Wall -Wextra -Werror -pedantic \
    "$ROOT/Driver/YMFPCIPCM.cpp" "$ROOT/Tests/pcm_test.cpp" -o "$TEMP/pcm_test"
"$TEMP/pcm_test"
"$PYTHON" "$ROOT/Tests/project_test.py"
# An invalid input must fail and must not truncate the previous output.
cp "$TEMP/YMFPCIFirmware.cpp" "$TEMP/expected.cpp"
printf x > "$TEMP/invalid.fw"
if "$PYTHON" "$ROOT/Tools/embed_firmware.py" "$TEMP/invalid.fw" \
    "$ROOT/Reference/Firmware/ds1e_ctrl.fw" "$TEMP/YMFPCIFirmware.cpp"; then
    echo "FAIL: malformed firmware accepted" >&2
    exit 1
fi
cmp "$TEMP/expected.cpp" "$TEMP/YMFPCIFirmware.cpp"
echo "PASS: failed firmware generation preserves existing output"
