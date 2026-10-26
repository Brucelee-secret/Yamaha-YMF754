/*
 * Firmware bytes are supplied by the user's archive, not relicensed here.
 * Only DS-1E images requested by ALSA for YMF754 are embedded.
 */
#ifndef YMFPCI_FIRMWARE_H
#define YMFPCI_FIRMWARE_H

#include "YMFPCIHardware.h"

extern const YMFByte gYMFDSPFirmware[];
extern const YMFByte gYMFControllerFirmware[];
extern const unsigned gYMFDSPFirmwareSize;
extern const unsigned gYMFControllerFirmwareSize;

#endif
