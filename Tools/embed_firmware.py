#!/usr/bin/env python
"""Generate the checked-in C++ source containing the supplied firmware."""
from __future__ import print_function
import os
import sys


def load(path, expected):
    with open(path, "rb") as source:
        data = bytearray(source.read())
    if len(data) != expected:
        raise ValueError("%s: got %d bytes, expected %d" %
                         (path, len(data), expected))
    return data


def render(name, data):
    lines = ["const YMFByte %s[] = {" % name]
    for offset in range(0, len(data), 12):
        lines.append("    " + ", ".join("0x%02x" % b
                     for b in data[offset:offset + 12]) + ",")
    lines.append("};")
    lines.append("const unsigned %sSize = %d;" % (name, len(data)))
    return "\n".join(lines) + "\n"


def main():
    if len(sys.argv) != 4:
        raise ValueError("usage: embed_firmware.py ds1_dsp.fw ds1e_ctrl.fw output.cpp")
    # Load both inputs before opening/truncating the output.
    dsp = load(sys.argv[1], 0x80)
    controller = load(sys.argv[2], 0x3000)
    output = sys.argv[3]
    directory = os.path.dirname(os.path.abspath(output))
    if not os.path.isdir(directory):
        os.makedirs(directory)
    text = ("#include \"YMFPCIFirmware.h\"\n"
            "/* Generated firmware data; do not edit or relicense the bytes. */\n")
    text += render("gYMFDSPFirmware", dsp)
    text += render("gYMFControllerFirmware", controller)
    temporary = output + ".tmp"
    with open(temporary, "w") as dest:
        dest.write(text)
    os.rename(temporary, output)
    print("Embedded DS-1E firmware: %d + %d bytes" % (len(dsp), len(controller)))


if __name__ == "__main__":
    try:
        main()
    except (IOError, OSError, ValueError) as error:
        sys.stderr.write("Firmware generation failed: %s\n" % error)
        sys.exit(1)
