/*
 * SPDX-License-Identifier: GPL-2.0-or-later
 * Derived from ALSA YMFPCI, copyright Jaroslav Kysela.
 */
#include "YMFPCIPCM.h"
#include "YMFPCIRegisters.h"

typedef char YMFPlaybackBankMustBe120Bytes[sizeof(YMFPlaybackBank) == 120 ? 1 : -1];

static unsigned align256(unsigned n) { return (n + 255U) & ~255U; }

bool ymfPCMLayout(unsigned playWords, unsigned captureWords,
    unsigned effectWords, YMFPCMLayout &layout)
{
    /* Firmware, not a guessed C struct size, determines bank-to-bank stride.
     * Reject absent hardware, impossible sizes and arithmetic overflow. */
    if (playWords < sizeof(YMFPlaybackBank) / 4 || playWords > 256 ||
        captureWords < 4 || captureWords > 256 ||
        effectWords < 4 || effectWords > 256)
        return false;
    layout.playbackStride = playWords * 4;
    layout.captureStride = captureWords * 4;
    layout.effectStride = effectWords * 4;
    layout.playbackOffset = align256((YMF_PCM_VOICES + 1) * 4);
    layout.captureOffset = layout.playbackOffset +
        align256(layout.playbackStride * 2 * YMF_PCM_VOICES);
    layout.effectOffset = layout.captureOffset + align256(layout.captureStride * 2 * 2);
    layout.workOffset = layout.effectOffset + align256(layout.effectStride * 2 * 5);
    layout.totalBytes = layout.workOffset + YMF_PCM_WORK_BYTES;
    return true;
}

void ymfPCMInitBank(YMFPlaybackBank &bank, unsigned channel, YMFDword sampleAddress)
{
    /* Caller zeros the complete firmware-sized bank, including extensions. */
    bank.format = 0x00010000U | (channel & 1U); /* interleaved S16, L/R */
    bank.base = sampleAddress;
    bank.loopEnd = YMF_PCM_FRAMES; /* frames, not bytes */
    bank.delta = bank.deltaEnd = 0x10000000; /* ALSA calc_delta(48000) */
    bank.lpfK = bank.lpfKEnd = bank.lpfQ = 0x40000000;
    bank.egGain = bank.egGainEnd = 0x40000000;
    if (channel == 0)
        bank.leftGain = bank.leftGainEnd = 0x40000000;
    else
        bank.rightGain = bank.rightGainEnd = 0x40000000;
}

void ymfPCMInitCaptureBank(YMFPCaptureBank &bank, YMFDword sampleAddress,
    unsigned loopEndBytes)
{
    bank.base = sampleAddress;
    bank.loopEnd = loopEndBytes;
    bank.start = 0;
    bank.numOfLoops = 0;
}

YMFDword ymfPCMS16ToFloatBits(short sample)
{
    if (sample == 0)
        return 0;
    const bool negative = sample < 0;
    const unsigned magnitude = negative ?
        static_cast<unsigned>(-static_cast<int>(sample)) :
        static_cast<unsigned>(sample);
    unsigned topBit = 0;
    for (unsigned value = magnitude; value >>= 1; )
        ++topBit;
    const unsigned significand = magnitude << (23 - topBit);
    return (negative ? 0x80000000U : 0) |
        ((topBit + 112U) << 23) | (significand & 0x007fffffU);
}

bool ymfAC97MasterVolumeRegister(unsigned value, YMFWord &regValue,
    bool muted)
{
    if (value > YMF_AC97_VOLUME_MAX)
        return false;
    if (value == YMF_AC97_VOLUME_MIN) {
        regValue = 0x9f1f; /* mute plus maximum attenuation on both channels */
        return true;
    }
    const YMFWord attenuation = static_cast<YMFWord>(YMF_AC97_VOLUME_MAX - value);
    regValue = static_cast<YMFWord>(attenuation | (attenuation << 8));
    if (muted)
        regValue = static_cast<YMFWord>(regValue | 0x8000U);
    return true;
}

bool ymfAC97MicVolumeRegister(unsigned value, YMFWord &regValue)
{
    if (value > YMF_AC97_MIC_VOLUME_MAX)
        return false;
    /* Keep the empirically working monitor-mute and +20 dB boost bits.
     * The 5-bit AC97 mic attenuator is inverse to the user-facing level. */
    const YMFWord attenuation = static_cast<YMFWord>(
        YMF_AC97_MIC_VOLUME_MAX - value);
    regValue = static_cast<YMFWord>(0x8040U | attenuation);
    return true;
}

bool ymfAC97ThreeDWideRegisters(YMFWord current3D,
    YMFWord currentGeneralPurpose, bool enabled, YMFWord &new3D,
    YMFWord &newGeneralPurpose)
{
    if (current3D == 0xffff || currentGeneralPurpose == 0xffff)
        return false;
    new3D = enabled ? YMF_AC97_3D_CONTROL_WIDE_VALUE : 0;
    newGeneralPurpose = enabled ?
        static_cast<YMFWord>(currentGeneralPurpose | YMF_AC97_3D_ENABLE_MASK) :
        static_cast<YMFWord>(currentGeneralPurpose & ~YMF_AC97_3D_ENABLE_MASK);
    return true;
}

short ymfPCMClipFloatBits(YMFDword bits)
{
    const unsigned exponent = (bits >> 23) & 255U;
    const bool negative = (bits & 0x80000000U) != 0;
    if (exponent == 255 && (bits & 0x007fffffU))
        return 0; /* NaN */
    if (exponent >= 127)
        return negative ? -32768 : 32767; /* >= 1, infinities */
    if (exponent < 112)
        return 0; /* includes zero, denormals, < one S16 step */
    const unsigned magnitude = ((bits & 0x007fffffU) | 0x00800000U) >>
        (135 - exponent);
    return static_cast<short>(negative ? -static_cast<int>(magnitude) :
        static_cast<int>(magnitude));
}

static YMFPCIResult codecReady(const YMFPCIHardwareIO &io)
{
    if (!io.context || !io.read16 || !io.sleepMilliseconds)
        return YMFPCI_BAD_ARGUMENT;
    for (unsigned i = 0; i < 750; ++i) {
        const YMFWord status = io.read16(io.context, YMF_REG_PRISTATUSADR);
        if (status == 0xffff)
            return YMFPCI_DEVICE_UNAVAILABLE;
        if (!(status & 0x8000))
            return YMFPCI_OK;
        io.sleepMilliseconds(io.context, 1);
    }
    return YMFPCI_CODEC_TIMEOUT;
}

YMFPCIResult ymfCodecRead(const YMFPCIHardwareIO &io, YMFWord reg, YMFWord &value)
{
    if ((reg & 1) || reg > 0x7e || !io.write16)
        return YMFPCI_BAD_ARGUMENT;
    YMFPCIResult result = codecReady(io);
    if (result != YMFPCI_OK)
        return result;
    io.write16(io.context, YMF_REG_AC97CMDADR, 0x8000 | reg);
    result = codecReady(io);
    if (result == YMFPCI_OK)
        value = io.read16(io.context, YMF_REG_PRISTATUSDATA);
    return result;
}

YMFPCIResult ymfCodecWrite(const YMFPCIHardwareIO &io, YMFWord reg, YMFWord value)
{
    if ((reg & 1) || reg > 0x7e || !io.write32)
        return YMFPCI_BAD_ARGUMENT;
    YMFPCIResult result = codecReady(io);
    if (result != YMFPCI_OK)
        return result;
    io.write32(io.context, YMF_REG_AC97CMDDATA,
        (static_cast<YMFDword>(reg) << 16) | value);
    return codecReady(io); /* drain/finish command before next operation */
}
