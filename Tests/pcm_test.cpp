/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "../Driver/YMFPCIPCM.h"
#include "../Driver/YMFPCIRegisters.h"
#include <cassert>
#include <cstddef>
#include <cstring>
#include <iostream>

struct Codec {
    YMFWord data, address;
    YMFDword command;
    unsigned slept;
    bool busy, absent;
    Codec() : data(0x0808), address(0), command(0), slept(0), busy(false), absent(false) {}
    static YMFWord read(void *p, unsigned o) {
        Codec &c = *static_cast<Codec *>(p);
        if (c.absent) return 0xffff;
        return o == YMF_REG_PRISTATUSADR ? (c.busy ? 0x8000 : 0) : c.data;
    }
    static void w16(void *p, unsigned o, YMFWord v) {
        assert(o == YMF_REG_AC97CMDADR);
        static_cast<Codec *>(p)->address = v;
    }
    static void w32(void *p, unsigned o, YMFDword v) {
        assert(o == YMF_REG_AC97CMDDATA);
        static_cast<Codec *>(p)->command = v;
    }
    static void sleep(void *p, unsigned ms) { static_cast<Codec *>(p)->slept += ms; }
    YMFPCIHardwareIO io() {
        YMFPCIHardwareIO value;
        std::memset(&value, 0, sizeof(value));
        value.context = this; value.read16 = read; value.write16 = w16;
        value.write32 = w32; value.sleepMilliseconds = sleep;
        return value;
    }
};

int main()
{
    assert(YMF_SPDIF_DEFAULT_STATUS == 0x8200);
    assert(YMF_REG_SAMPLECOUNT == 0x010C);
    assert(YMF_REG_NUMOFSAMPLES == 0x0110);
    assert(sizeof(YMFPlaybackBank) == 120);
    assert(offsetof(YMFPlaybackBank, start) == 0x48);
    assert(offsetof(YMFPlaybackBank, delta) == 0x50);
    assert(YMF_REG_ADCSLOTSR == 0x00C0);
    assert(YMF_REG_ADCFORMAT == 0x00C8);
    assert(YMF_PCM_MIC_CAPTURE_VOICE == 1);
    assert(YMF_AC97_GENERAL_PURPOSE_REGISTER == 0x20);
    assert(YMF_AC97_3D_CONTROL_REGISTER == 0x22);
    assert(YMF_AC97_3D_ENABLE_MASK == 0x2000);
    assert(YMF_AC97_3D_CONTROL_WIDE_VALUE == 0x0f0f);
    assert(YMF_AC97_3D_READBACK_MASK == 0x000f);
    assert(YMF_AC97_3D_READBACK_WIDE_VALUE == 0x000f);
    YMFPCMLayout layout;
    assert(ymfPCMLayout(30, 4, 4, layout));
    assert(layout.playbackStride == 120 && layout.playbackOffset == 512);
    assert(layout.captureOffset == 15872 && layout.effectOffset == 16128);
    assert(layout.workOffset == 16384 && layout.totalBytes == 17408);
    assert(ymfPCMLayout(32, 8, 8, layout));
    assert(layout.playbackStride == 128);
    for (unsigned v = 0; v < 64; ++v)
        for (unsigned b = 0; b < 2; ++b)
            assert(layout.playbackOffset + (v * 2 + b + 1) * layout.playbackStride
                <= layout.captureOffset);
    assert(!ymfPCMLayout(29, 4, 4, layout));
    assert(!ymfPCMLayout(30, 0, 4, layout));
    assert(!ymfPCMLayout(30, 4, 0xffffffffU, layout));
    assert(!ymfPCMLayout(0xffffffffU, 4, 4, layout));

    for (unsigned channel = 0; channel < 2; ++channel) {
        YMFPlaybackBank bank;
        std::memset(&bank, 0, sizeof(bank));
        ymfPCMInitBank(bank, channel, 0x12340000);
        assert(bank.format == (0x10000 | channel));
        assert(bank.base == 0x12340000 && bank.loopEnd == 16384);
        assert(bank.delta == 0x10000000 && bank.deltaEnd == bank.delta);
        assert(bank.leftGain == (channel == 0 ? 0x40000000U : 0));
        assert(bank.rightGain == (channel == 1 ? 0x40000000U : 0));
        assert(!bank.start && !bank.startFrac && !bank.eff1Gain);
    }
    YMFPCaptureBank capture;
    std::memset(&capture, 0xff, sizeof(capture));
    ymfPCMInitCaptureBank(capture, 0x56780000, YMF_PCM_BYTES);
    assert(capture.base == 0x56780000 && capture.loopEnd == YMF_PCM_BYTES);
    assert(capture.start == 0 && capture.numOfLoops == 0);
    assert(ymfPCMS16ToFloatBits(0) == 0x00000000U);
    assert(ymfPCMS16ToFloatBits(16384) == 0x3f000000U);
    assert(ymfPCMS16ToFloatBits(-16384) == 0xbf000000U);
    assert(ymfPCMS16ToFloatBits(32767) == 0x3f7ffe00U);
    assert(ymfPCMS16ToFloatBits(-32768) == 0xbf800000U);
    assert(ymfPCMS16ToFloatBits(-1) == 0xb8000000U);
    for (int sample = -32768; sample <= 32767; ++sample) {
        const float normalized = static_cast<float>(sample) / 32768.0f;
        YMFDword expected;
        std::memcpy(&expected, &normalized, sizeof(expected));
        assert(ymfPCMS16ToFloatBits(static_cast<short>(sample)) == expected);
    }

    assert(ymfPCMClipFloatBits(0) == 0);
    assert(ymfPCMClipFloatBits(0x80000000) == 0);
    assert(ymfPCMClipFloatBits(0x3f000000) == 16384); /* 0.5 */
    assert(ymfPCMClipFloatBits(0xbf000000) == -16384);
    assert(ymfPCMClipFloatBits(0x3f800000) == 32767);
    assert(ymfPCMClipFloatBits(0xbf800000) == -32768);
    assert(ymfPCMClipFloatBits(0x7f800000) == 32767);
    assert(ymfPCMClipFloatBits(0xff800000) == -32768);
    assert(ymfPCMClipFloatBits(0x7fc00000) == 0);
    assert(ymfPCMClipFloatBits(1) == 0);
    assert(ymfPCMClipFloatBits(0x38000000) == 1); /* 2^-15 */
    YMFWord masterVolume = 0xffff;
    assert(YMF_AC97_VOLUME_INITIAL == 16);
    assert(ymfAC97MasterVolumeRegister(YMF_AC97_VOLUME_MIN, masterVolume));
    assert(masterVolume == 0x9f1f);
    assert(ymfAC97MasterVolumeRegister(YMF_AC97_VOLUME_INITIAL, masterVolume));
    assert(masterVolume == 0x0f0f); /* conservative startup level: -22.5 dB */
    assert(ymfAC97MasterVolumeRegister(
        YMF_AC97_VOLUME_INITIAL, masterVolume, true));
    assert(masterVolume == 0x8f0f); /* mute preserves the selected level */
    assert(ymfAC97MasterVolumeRegister(YMF_AC97_VOLUME_MAX, masterVolume));
    assert(masterVolume == 0); /* 0 dB */
    assert(ymfAC97MasterVolumeRegister(
        YMF_AC97_VOLUME_MAX, masterVolume, true));
    assert(masterVolume == 0x8000); /* explicit mute at full slider level */
    assert(ymfAC97MasterVolumeRegister(
        YMF_AC97_VOLUME_INITIAL, masterVolume, false));
    assert(masterVolume == 0x0f0f); /* unmute restores the slider level */
    masterVolume = 0x1234;
    assert(!ymfAC97MasterVolumeRegister(YMF_AC97_VOLUME_MAX + 1, masterVolume));
    assert(masterVolume == 0x1234); /* reject without changing caller state */
    YMFWord micVolume = 0xffff;
    assert(ymfAC97MicVolumeRegister(YMF_AC97_MIC_VOLUME_MIN, micVolume));
    assert(micVolume == 0x805f); /* quietest, monitor muted, boost retained */
    assert(ymfAC97MicVolumeRegister(16, micVolume));
    assert(micVolume == 0x804f);
    assert(ymfAC97MicVolumeRegister(YMF_AC97_MIC_VOLUME_INITIAL, micVolume));
    assert(micVolume == 0x804f); /* quieter startup default requested by user */
    assert(ymfAC97MicVolumeRegister(YMF_AC97_MIC_VOLUME_MAX, micVolume));
    assert(micVolume == 0x8040); /* current confirmed working sensitivity */
    micVolume = 0x1234;
    assert(!ymfAC97MicVolumeRegister(YMF_AC97_MIC_VOLUME_MAX + 1, micVolume));
    assert(micVolume == 0x1234);
    YMFWord wide3D = 0xffff, generalPurpose = 0xffff;
    assert(ymfAC97ThreeDWideRegisters(0xa5a3, 0x1234, true,
        wide3D, generalPurpose));
    assert(wide3D == 0x0f0f);
    assert(generalPurpose == 0x3234); /* preserve unrelated GP bits */
    assert(ymfAC97ThreeDWideRegisters(0x000f, 0x3234, false,
        wide3D, generalPurpose));
    assert(wide3D == 0 && generalPurpose == 0x1234);
    wide3D = 0x4321;
    generalPurpose = 0x1234;
    assert(!ymfAC97ThreeDWideRegisters(0xffff, 0x1234, true,
        wide3D, generalPurpose));
    assert(wide3D == 0x4321 && generalPurpose == 0x1234);
    /* Compare a wide range of IEEE samples against a userspace FPU oracle.
     * The driver implementation itself uses integer instructions only. */
    for (YMFDword bits = 0x30000000; bits < 0x41000000; bits += 997) {
        for (unsigned sign = 0; sign < 2; ++sign) {
            YMFDword raw = bits | (sign ? 0x80000000U : 0);
            float f;
            std::memcpy(&f, &raw, sizeof(f));
            int expected = f >= 1 ? 32767 : f <= -1 ? -32768 :
                static_cast<int>(f * 32768.0f);
            assert(ymfPCMClipFloatBits(raw) == expected);
        }
    }
    Codec codec;
    YMFWord value = 0;
    assert(ymfCodecRead(codec.io(), 0x02, value) == YMFPCI_OK);
    assert(codec.address == 0x8002 && value == 0x0808);
    assert(ymfCodecWrite(codec.io(), 0x18, 0x1234) == YMFPCI_OK);
    assert(codec.command == 0x00181234);
    assert(ymfCodecWrite(codec.io(), 1, 0) == YMFPCI_BAD_ARGUMENT);
    codec.busy = true;
    assert(ymfCodecRead(codec.io(), 0x02, value) == YMFPCI_CODEC_TIMEOUT);
    assert(codec.slept == 750);
    codec.absent = true;
    assert(ymfCodecWrite(codec.io(), 0x18, 0) == YMFPCI_DEVICE_UNAVAILABLE);
    std::cout << "PASS: playback/capture DMA banks, stereo S16/48k routing, integer Float32 conversion, AC97 commands/timeouts\n";
}
