/*
 * SPDX-License-Identifier: GPL-2.0-or-later
 * Hardware sequence ported from ALSA ymfpci_main.c by Jaroslav Kysela.
 * This interface deliberately has no Linux or IOKit dependencies.
 */
#ifndef YMFPCI_HARDWARE_H
#define YMFPCI_HARDWARE_H

typedef unsigned char YMFByte;
typedef unsigned short YMFWord;
typedef unsigned int YMFDword;

struct YMFPCIHardwareIO {
    void *context;
    YMFWord (*read16)(void *, unsigned);
    YMFDword (*read32)(void *, unsigned);
    void (*write16)(void *, unsigned, YMFWord);
    void (*write32)(void *, unsigned, YMFDword);
    YMFByte (*configRead8)(void *, unsigned);
    void (*configWrite8)(void *, unsigned, YMFByte);
    void (*configWrite16)(void *, unsigned, YMFWord);
    void (*delayMicroseconds)(void *, unsigned);
    void (*sleepMilliseconds)(void *, unsigned);
};

enum YMFPCIResult {
    YMFPCI_OK = 0,
    YMFPCI_BAD_ARGUMENT,
    YMFPCI_BAD_FIRMWARE_SIZE,
    YMFPCI_DEVICE_UNAVAILABLE,
    YMFPCI_CODEC_TIMEOUT,
    YMFPCI_DSP_STOP_TIMEOUT,
    YMFPCI_ENGINE_STOP_TIMEOUT
};

/* Uploads images but does NOT enable DSP, DMA, interrupts, or audio streams. */
YMFPCIResult ymfUploadFirmware(const YMFPCIHardwareIO &io,
    const YMFByte *dsp, unsigned dspSize,
    const YMFByte *controller, unsigned controllerSize);

/* Bounded stop; caller must have memory decoding enabled and BAR0 mapped. */
YMFPCIResult ymfStopDSP(const YMFPCIHardwareIO &io);
/* Stop playback MODE and let the enabled DSP finish its current bank before
 * disabling PCI bus mastering. Leaves CONFIG/DSP enabled for a later restart. */
YMFPCIResult ymfStopPCM(const YMFPCIHardwareIO &io);
const char *ymfResultName(YMFPCIResult result);

#endif
