/*
 * SPDX-License-Identifier: GPL-2.0-or-later
 * Hardware ABI derived from ALSA YMFPCI, copyright Jaroslav Kysela.
 */
#ifndef YMFPCI_PCM_H
#define YMFPCI_PCM_H
#include "YMFPCIHardware.h"

enum {
    YMF_PCM_RATE = 48000,
    YMF_PCM_FRAMES = 16384,
    YMF_PCM_BYTES = YMF_PCM_FRAMES * 4,
    YMF_PCM_VOICES = 64,
    YMF_PCM_CAPTURE_VOICES = 2,
    YMF_PCM_MIC_CAPTURE_VOICE = 1,
    YMF_PCM_CAPTURE_STEREO_FORMAT = 2,
    YMF_PCM_ADC_48K_SLOT_RATE = 0x0fff,
    YMF_PCM_WORK_BYTES = 0x400,
    YMF_AC97_VOLUME_MIN = 0,
    YMF_AC97_VOLUME_MAX = 31,
    YMF_AC97_VOLUME_INITIAL = 16,
    YMF_AC97_MIC_VOLUME_MIN = 0,
    YMF_AC97_MIC_VOLUME_MAX = 31,
    YMF_AC97_MIC_VOLUME_INITIAL = 16,
    YMF_AC97_GENERAL_PURPOSE_REGISTER = 0x20,
    YMF_AC97_3D_CONTROL_REGISTER = 0x22,
    YMF_AC97_3D_ENABLE_MASK = 0x2000,
    YMF_AC97_3D_CONTROL_WIDE_VALUE = 0x0f0f,
    YMF_AC97_3D_READBACK_MASK = 0x000f,
    YMF_AC97_3D_READBACK_WIDE_VALUE = 0x000f
};

/* Every field is a little-endian hardware dword; native on our x86_64 target.
 * Do not replace this with a compiler/OS-specific audio structure. */
struct YMFPlaybackBank {
    YMFDword format, loopDefault, base, loopStart, loopEnd, loopFrac;
    YMFDword deltaEnd, lpfKEnd, egGainEnd, leftGainEnd, rightGainEnd;
    YMFDword eff1GainEnd, eff2GainEnd, eff3GainEnd, lpfQ, status;
    YMFDword numOfFrames, loopCount, start, startFrac, delta, lpfK;
    YMFDword egGain, leftGain, rightGain, eff1Gain, eff2Gain, eff3Gain;
    YMFDword lpfD1, lpfD2;
};

struct YMFPCaptureBank {
    YMFDword base, loopEnd, start, numOfLoops;
};

struct YMFPCMLayout {
    unsigned playbackStride, captureStride, effectStride;
    unsigned playbackOffset, captureOffset, effectOffset, workOffset, totalBytes;
};

bool ymfPCMLayout(unsigned playWords, unsigned captureWords,
    unsigned effectWords, YMFPCMLayout &layout);
void ymfPCMInitBank(YMFPlaybackBank &bank, unsigned channel,
    YMFDword sampleAddress);
void ymfPCMInitCaptureBank(YMFPCaptureBank &bank, YMFDword sampleAddress,
    unsigned loopEndBytes);
/* Convert CoreAudio IEEE-754 Float32 bits without executing FPU/SSE in kernel. */
short ymfPCMClipFloatBits(YMFDword bits);
/* Convert signed 16-bit PCM to normalized IEEE-754 Float32 bits without FPU. */
YMFDword ymfPCMS16ToFloatBits(short sample);
bool ymfAC97MasterVolumeRegister(unsigned value, YMFWord &regValue,
    bool muted = false);
bool ymfAC97MicVolumeRegister(unsigned value, YMFWord &regValue);
bool ymfAC97ThreeDWideRegisters(YMFWord current3D, YMFWord currentGeneralPurpose,
    bool enabled, YMFWord &new3D, YMFWord &newGeneralPurpose);
YMFPCIResult ymfCodecRead(const YMFPCIHardwareIO &io, YMFWord reg, YMFWord &value);
YMFPCIResult ymfCodecWrite(const YMFPCIHardwareIO &io, YMFWord reg, YMFWord value);
#endif
