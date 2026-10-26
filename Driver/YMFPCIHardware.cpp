/*
 * SPDX-License-Identifier: GPL-2.0-or-later
 * Derived from ALSA YMFPCI initialization, copyright Jaroslav Kysela.
 * Transport/delays and explicit failure handling are supplied by the host.
 */
#include "YMFPCIHardware.h"
#include "YMFPCIRegisters.h"

static bool validIO(const YMFPCIHardwareIO &io)
{
    return io.context && io.read16 && io.read32 && io.write16 && io.write32 &&
        io.configRead8 && io.configWrite8 && io.configWrite16 &&
        io.delayMicroseconds && io.sleepMilliseconds;
}

static YMFDword littleWord(const YMFByte *p)
{
    return (YMFDword)p[0] | ((YMFDword)p[1] << 8) |
        ((YMFDword)p[2] << 16) | ((YMFDword)p[3] << 24);
}

YMFPCIResult ymfStopPCM(const YMFPCIHardwareIO &io)
{
    if (!validIO(io))
        return YMFPCI_BAD_ARGUMENT;
    const YMFDword config = io.read32(io.context, YMF_REG_CONFIG);
    const YMFDword mode = io.read32(io.context, YMF_REG_MODE);
    if (config == 0xffffffffU || mode == 0xffffffffU)
        return YMFPCI_DEVICE_UNAVAILABLE;

    /* Match ALSA snd_ymfpci_hw_stop: stop issuing new voice work, then let the
     * still-enabled DSP complete the current bank. Clearing CONFIG first can
     * strand STATUS.busy asserted and makes the following engine start fail. */
    io.write32(io.context, YMF_REG_STATUS, 0x80000000U);
    io.write32(io.context, YMF_REG_MODE, mode & ~3U);
    const YMFDword stoppedMode = io.read32(io.context, YMF_REG_MODE);
    if (stoppedMode == 0xffffffffU)
        return YMFPCI_DEVICE_UNAVAILABLE;
    if (stoppedMode & 3U)
        return YMFPCI_ENGINE_STOP_TIMEOUT;

    /* 20 ms accommodates several 256-frame quanta at 48 kHz without sleeping
     * in the engine-stop context. The buffers remain mapped throughout. */
    for (unsigned i = 0; i < 400; ++i) {
        const YMFDword status = io.read32(io.context, YMF_REG_STATUS);
        if (status == 0xffffffffU)
            return YMFPCI_DEVICE_UNAVAILABLE;
        if ((status & 2U) == 0) {
            io.write32(io.context, YMF_REG_STATUS, 0x80000000U);
            io.read32(io.context, YMF_REG_STATUS);
            return YMFPCI_OK;
        }
        io.delayMicroseconds(io.context, 50);
    }
    return YMFPCI_ENGINE_STOP_TIMEOUT;
}

YMFPCIResult ymfStopDSP(const YMFPCIHardwareIO &io)
{
    if (!validIO(io))
        return YMFPCI_BAD_ARGUMENT;
    const YMFDword config = io.read32(io.context, YMF_REG_CONFIG);
    if (config == 0xffffffffU)
        return YMFPCI_DEVICE_UNAVAILABLE;
    if (config != 0) {
        const YMFPCIResult quiesced = ymfStopPCM(io);
        if (quiesced != YMFPCI_OK)
            return quiesced;
    }
    io.write32(io.context, YMF_REG_CONFIG, 0);
    /* Read back to drain the posted PCI write before polling STATUS. */
    const YMFDword stoppedConfig = io.read32(io.context, YMF_REG_CONFIG);
    if (stoppedConfig == 0xffffffffU)
        return YMFPCI_DEVICE_UNAVAILABLE;
    if (stoppedConfig != 0)
        return YMFPCI_DSP_STOP_TIMEOUT;
    for (unsigned i = 0; i < 1000; ++i) {
        const YMFDword status = io.read32(io.context, YMF_REG_STATUS);
        if (status == 0xffffffffU)
            return YMFPCI_DEVICE_UNAVAILABLE;
        if ((status & 2U) == 0)
            return YMFPCI_OK;
        io.delayMicroseconds(io.context, 1);
    }
    return YMFPCI_DSP_STOP_TIMEOUT;
}

YMFPCIResult ymfUploadFirmware(const YMFPCIHardwareIO &io,
    const YMFByte *dsp, unsigned dspSize,
    const YMFByte *controller, unsigned controllerSize)
{
    if (!validIO(io) || !dsp || !controller)
        return YMFPCI_BAD_ARGUMENT;
    /* Validate both images BEFORE any register or PCI configuration write. */
    if (dspSize != YMF_DSP_LENGTH || controllerSize != YMF_CTRL_LENGTH)
        return YMFPCI_BAD_FIRMWARE_SIZE;

    const YMFByte control = io.configRead8(io.context, YMF_PCI_DSXG_CTRL);
    if (control == 0xff)
        return YMFPCI_DEVICE_UNAVAILABLE;

    /* snd_ymfpci_aclink_reset: preserve unrelated DSXG_CTRL bits. */
    io.configWrite8(io.context, YMF_PCI_DSXG_CTRL, control & 0xfc);
    io.configWrite8(io.context, YMF_PCI_DSXG_CTRL, control | 0x03);
    io.configWrite8(io.context, YMF_PCI_DSXG_CTRL, control & 0xfc);
    io.configWrite16(io.context, YMF_PCI_PWRCTRL1, 0);
    io.configWrite16(io.context, YMF_PCI_PWRCTRL2, 0);

    bool ready = false;
    for (unsigned i = 0; i < 750; ++i) {
        const YMFWord status = io.read16(io.context, YMF_REG_PRISTATUSADR);
        if (status == 0xffff)
            return YMFPCI_DEVICE_UNAVAILABLE;
        if ((status & 0x8000) == 0) {
            ready = true;
            break;
        }
        io.sleepMilliseconds(io.context, 1);
    }
    if (!ready)
        return YMFPCI_CODEC_TIMEOUT;

    io.write32(io.context, YMF_REG_NATIVEDACOUTVOL, 0);
    const YMFPCIResult stopped = ymfStopDSP(io);
    if (stopped != YMFPCI_OK)
        return stopped; /* Never overwrite instruction RAM while DSP is busy. */

    io.write32(io.context, YMF_REG_MODE, 0x00010000);
    io.write32(io.context, YMF_REG_MODE, 0);
    io.write32(io.context, YMF_REG_MAPOFREC, 0);
    io.write32(io.context, YMF_REG_MAPOFEFFECT, 0);
    io.write32(io.context, YMF_REG_PLAYCTRLBASE, 0);
    io.write32(io.context, YMF_REG_RECCTRLBASE, 0);
    io.write32(io.context, YMF_REG_EFFCTRLBASE, 0);
    io.write32(io.context, YMF_REG_WORKBASE, 0);
    io.write32(io.context, YMF_REG_WORKSIZE, 0);
    const YMFWord global = io.read16(io.context, YMF_REG_GLOBALCTRL);
    if (global == 0xffff)
        return YMFPCI_DEVICE_UNAVAILABLE;
    io.write16(io.context, YMF_REG_GLOBALCTRL, global & ~7U);

    for (unsigned offset = 0; offset < dspSize; offset += 4)
        io.write32(io.context, YMF_REG_DSPINSTRAM + offset,
            littleWord(dsp + offset));
    for (unsigned offset = 0; offset < controllerSize; offset += 4)
        io.write32(io.context, YMF_REG_CTRLINSTRAM + offset,
            littleWord(controller + offset));

    /* Flush instruction writes; ALSA enables CONFIG here, this stage does not. */
    if (io.read32(io.context, YMF_REG_CONFIG) != 0)
        return YMFPCI_DEVICE_UNAVAILABLE;
    io.delayMicroseconds(io.context, 100);
    return YMFPCI_OK;
}

const char *ymfResultName(YMFPCIResult result)
{
    switch (result) {
    case YMFPCI_OK: return "ok (firmware uploaded, DSP stopped)";
    case YMFPCI_BAD_ARGUMENT: return "invalid transport or firmware pointer";
    case YMFPCI_BAD_FIRMWARE_SIZE: return "wrong firmware size";
    case YMFPCI_DEVICE_UNAVAILABLE: return "device unavailable or unexpected register state";
    case YMFPCI_CODEC_TIMEOUT: return "primary AC97 codec readiness timeout";
    case YMFPCI_DSP_STOP_TIMEOUT: return "DSP stop timeout";
    case YMFPCI_ENGINE_STOP_TIMEOUT: return "PCM engine idle timeout";
    default: return "unknown error";
    }
}
