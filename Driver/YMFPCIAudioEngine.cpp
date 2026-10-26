/*
 * SPDX-License-Identifier: GPL-2.0-or-later
 * Hardware sequences derived from ALSA YMFPCI, copyright Jaroslav Kysela.
 */
#include "YMFPCIAudioEngine.h"
#include "YMFPCIAudioDevice.h"
#include "YMFPCIRegisters.h"
#include "YMFPCIFirmware.h"
#include <IOKit/audio/IOAudioStream.h>
#include <IOKit/audio/IOAudioTypes.h>
#include <IOKit/audio/IOAudioLevelControl.h>
#include <IOKit/audio/IOAudioSelectorControl.h>
#include <IOKit/audio/IOAudioToggleControl.h>
#include <IOKit/IOCommandGate.h>
#include <IOKit/IOLib.h>
#include <libkern/libkern.h>

enum {
    YMF_AUDIO_CONTROL_ID_3D_WIDE = 0x33445744U
};

/* Input conversion writes one callback-sized block to IOAudioFamily's mix
 * buffer; firstFrame indexes the hardware capture ring, not that block. */
#define super IOAudioEngine
OSDefineMetaClassAndStructors(YMFPCIAudioEngine, IOAudioEngine)
OSDefineMetaClassAndStructors(YMFPCIMicEngine, IOAudioEngine)

static inline void dmaFence()
{
    /* x86_64 coherent DMA: publish WB descriptor/sample stores before MMIO,
     * and order subsequent CPU reads of DSP-written positions. No FPU/SSE. */
    __asm__ __volatile__("mfence" : : : "memory");
}

static bool supportedFormat(const IOAudioStreamFormat *format)
{
    return !format || (format->fNumChannels == 2 &&
        format->fSampleFormat == kIOAudioStreamSampleFormatLinearPCM &&
        format->fNumericRepresentation == kIOAudioStreamNumericRepresentationSignedInt &&
        format->fBitDepth == 16 && format->fBitWidth == 16 &&
        format->fByteOrder == kIOAudioStreamByteOrderLittleEndian &&
        format->fAlignment == kIOAudioStreamAlignmentLowByte &&
        format->fIsMixable == 1);
}

bool YMFPCIAudioEngine::init(OSDictionary *properties)
{
    fDevice = 0;
    fLock = 0;
    fLifecycleLock = 0;
    fMicEngine = 0;
    fReady = fRunning = fSuspended = fReportedIRQ = false;
    fReportedProgress = fReportedWrap = fReportedZeroFrame = fReportedPCMData = false;
    fReportedOtherIRQ = false;
    fReportedDSPState = false;
    fPlaybackActive = false;
    fCaptureActive = false;
    fOutputMuted = false;
    fDigitalOutput = false;
    /* Experimental default: use the full 3D Wide register sequence at startup. */
    fThreeDWide = true;
    fThreeDWideSupported = false;
    fLastFrame = fPendingDiagnostics = fDiagnosticFrames = 0;
    fPendingIRQStatus = 0;
    fPendingINTFlag = 0;
    fVolumeValue = YMF_AC97_VOLUME_INITIAL;
    fMicVolumeValue = YMF_AC97_MIC_VOLUME_INITIAL;
    bzero(&fLayout, sizeof(fLayout));
    bzero(&fControl, sizeof(fControl));
    bzero(&fSamples, sizeof(fSamples));
    bzero(&fCaptureSamples, sizeof(fCaptureSamples));
    fCaptureRunning = false;
    fCaptureDiagnosticFrames = 0;
    fCaptureDiagnosticNonzero = 0;
    fCaptureDiagnosticPeak = 0;
    if (!super::init(properties))
        return false;
    fLock = IOSimpleLockAlloc();
    fLifecycleLock = IOLockAlloc();
    return fLock != 0 && fLifecycleLock != 0;
}

void YMFPCIAudioEngine::setDigitalOutput(bool digital)
{
    fDigitalOutput = digital;
}

bool YMFPCIAudioEngine::start(IOService *provider)
{
    if (!super::start(provider))
        return false;
    /* Legacy IOAudioFamily versions can return true from start() even when
     * our initHardware() failed. Never register a nonfunctional engine. */
    if (!fReady) {
        shutdown();
        super::stop(provider);
        return false;
    }
    return true;
}

bool YMFPCIAudioEngine::discoverLayout(YMFPCMLayout &layout)
{
    const YMFPCIHardwareIO &io = fDevice->fHardwareIO;
    /* Firmware publishes bank sizes only while CONFIG=1. MODE stays zero
     * and PCI bus mastering stays off: there can be no unconfigured DMA. */
    io.write32(io.context, YMF_REG_MODE, 0);
    io.write32(io.context, YMF_REG_CONFIG, 1);
    io.read32(io.context, YMF_REG_CONFIG);
    bool valid = false;
    for (unsigned i = 0; i < 20; ++i) {
        IOSleep(1);
        if (ymfPCMLayout(io.read32(io.context, YMF_REG_PLAYCTRLSIZE),
            io.read32(io.context, YMF_REG_RECCTRLSIZE),
            io.read32(io.context, YMF_REG_EFFCTRLSIZE), layout)) {
            valid = true;
            break;
        }
    }
    const YMFPCIResult stopped = ymfStopDSP(io);
    if (!valid || stopped != YMFPCI_OK)
        IOLog("YMFPCI: firmware bank sizes invalid or DSP did not stop\n");
    return valid && stopped == YMFPCI_OK;
}

bool YMFPCIAudioEngine::initHardware(IOService *provider)
{
    if (!super::initHardware(provider))
        return false;
    fDevice = OSDynamicCast(YMFPCIAudioDevice, provider);
    if (!fDevice)
        return false;

    /*
     * Both output endpoints use the same firmware-defined bank layout. The
     * analog engine has already discovered it while initializing the device.
     * Re-running discoverLayout() here writes MODE/CONFIG and stops the DSP;
     * that silently invalidates the configured analog engine and its mic
     * path. Reuse the cached layout so registering S/PDIF is hardware-neutral.
     */
    if (fDigitalOutput) {
        YMFPCIAudioEngine *analogEngine = fDevice->fAudioEngine;
        if (!analogEngine || !analogEngine->fReady ||
            analogEngine->fLayout.totalBytes == 0)
            return false;
        fLayout = analogEngine->fLayout;
    } else if (!discoverLayout(fLayout)) {
        return false;
    }

    if (!allocateDMA(fControl, fLayout.totalBytes) ||
        !allocateDMA(fSamples, YMF_PCM_BYTES) ||
        (!fDigitalOutput && !allocateDMA(fCaptureSamples, YMF_PCM_BYTES)))
        return false;
    IOLog("YMFPCI: bank bytes play=%u rec=%u effect=%u; control=%u bytes\n",
        fLayout.playbackStride, fLayout.captureStride, fLayout.effectStride,
        fLayout.totalBytes);

    if (!fDigitalOutput && !configureCodec())
        return false;
    /* Record the codec state after our writes, still without changing it. */
    if (!fDigitalOutput)
        fDevice->logAC97Snapshot("post-codec-config");
    /* Keep the analog hardware state untouched while the second endpoint is
     * being registered. The digital engine's registers are programmed only
     * when CoreAudio actually starts an S/PDIF stream. */
    if (!fDigitalOutput && !configureHardware())
        return false;

    if (!fDigitalOutput) {
        IOAudioLevelControl *volume = IOAudioLevelControl::createVolumeControl(
            fVolumeValue, YMF_AC97_VOLUME_MIN, YMF_AC97_VOLUME_MAX,
            static_cast<IOFixed>(-3047424), 0, kIOAudioControlChannelIDAll, 0, 0,
            kIOAudioControlUsageOutput);
        if (!volume)
            return false;
        volume->setValueChangeHandler(volumeValueChanged, this);
        const IOReturn volumeResult = addDefaultAudioControl(volume);
        volume->release();
        if (volumeResult != kIOReturnSuccess)
            return false;

        IOAudioToggleControl *mute = IOAudioToggleControl::createMuteControl(
            fOutputMuted, kIOAudioControlChannelIDAll, 0, 0,
            kIOAudioControlUsageOutput);
        if (!mute)
            return false;
        mute->setValueChangeHandler(muteValueChanged, this);
        const IOReturn muteResult = addDefaultAudioControl(mute);
        mute->release();
        if (muteResult != kIOReturnSuccess)
            return false;

        if (fThreeDWideSupported) {
            IOAudioToggleControl *wide = IOAudioToggleControl::create(
                fThreeDWide, kIOAudioControlChannelIDAll, 0,
                YMF_AUDIO_CONTROL_ID_3D_WIDE, 0, kIOAudioControlUsageOutput);
            if (wide) {
                wide->setValueChangeHandler(threeDWideValueChanged, this);
                const IOReturn wideResult = addDefaultAudioControl(wide);
                wide->release();
                if (wideResult != kIOReturnSuccess)
                    IOLog("YMFPCI: 3D Wide toggle registration failed: 0x%08x\n",
                        wideResult);
            } else {
                IOLog("YMFPCI: could not allocate 3D Wide toggle control\n");
            }
        }
    }

    IOAudioStream *stream = new IOAudioStream;
    if (!stream)
        return false;
    bool ok = stream->initWithAudioEngine(this, kIOAudioStreamDirectionOutput, 1,
        fDigitalOutput ? "YMF754 S/PDIF stereo" : "YMF754 analog stereo");
    if (ok) {
        IOAudioStreamFormat format;
        bzero(&format, sizeof(format));
        format.fNumChannels = 2;
        format.fSampleFormat = kIOAudioStreamSampleFormatLinearPCM;
        format.fNumericRepresentation = kIOAudioStreamNumericRepresentationSignedInt;
        format.fBitDepth = format.fBitWidth = 16;
        format.fAlignment = kIOAudioStreamAlignmentLowByte;
        format.fByteOrder = kIOAudioStreamByteOrderLittleEndian;
        format.fIsMixable = 1;
        IOAudioSampleRate rate = { YMF_PCM_RATE, 0 };
        setDescription(fDigitalOutput ?
            "Yamaha YMF754 Digital Output (S/PDIF)" :
            "Yamaha YMF754 Analog Output");
        setNumSampleFramesPerBuffer(YMF_PCM_FRAMES);
        setSampleRate(&rate);
        setSampleOffset(256); /* one firmware quantum of write-ahead */
        stream->setSampleBuffer(fSamples.bytes, YMF_PCM_BYTES);
        stream->addAvailableFormat(&format, &rate, &rate);
        ok = stream->setFormat(&format, false) == kIOReturnSuccess &&
            addAudioStream(stream) == kIOReturnSuccess;
    }
    stream->release();
    if (!ok)
        return false;

    fReady = true;
    return true;
}

bool YMFPCIAudioEngine::configureCodec()
{
    const YMFPCIHardwareIO &io = fDevice->fHardwareIO;
    YMFWord vendor1 = 0, vendor2 = 0, power = 0, extended = 0;
    YMFWord codecCapabilities = 0xffff;
    if (ymfCodecWrite(io, 0x00, 0) != YMFPCI_OK)
        return false;
    IOSleep(10);
    if (ymfCodecRead(io, 0x7c, vendor1) != YMFPCI_OK ||
        ymfCodecRead(io, 0x7e, vendor2) != YMFPCI_OK ||
        (vendor1 == 0xffff && vendor2 == 0xffff) || (!vendor1 && !vendor2) ||
        ymfCodecWrite(io, 0x26, 0) != YMFPCI_OK)
        return false;
    const YMFPCIResult capabilitiesResult =
        ymfCodecRead(io, 0x00, codecCapabilities);
    if (capabilitiesResult == YMFPCI_OK)
        IOLog("YMFPCI: AC97 reset/capability register=%04x (raw)\n",
            codecCapabilities);
    else
        IOLog("YMFPCI: AC97 reset/capability register read failed: %s\n",
            ymfResultName(capabilitiesResult));
    /* The observed 3D Wide sequence is codec-specific; don't apply it to
     * other AC'97 codec vendors. */
    fThreeDWideSupported = vendor1 == 0x8384 && vendor2 == 0x7608;
    bool ready = false;
    for (unsigned i = 0; i < 500; ++i) {
        if (ymfCodecRead(io, 0x26, power) != YMFPCI_OK || power == 0xffff)
            break;
        if ((power & 0x0f) == 0x0f) {
            ready = true;
            break;
        }
        IOSleep(1);
    }
    if (!ready) {
        IOLog("YMFPCI: AC97 analog power-up timeout: 0x%04x\n", power);
        return false;
    }
    if (ymfCodecRead(io, 0x28, extended) != YMFPCI_OK)
        return false;
    if (extended != 0xffff && (extended & 7) &&
        (ymfCodecRead(io, 0x2a, extended) != YMFPCI_OK ||
         ymfCodecWrite(io, 0x2a, extended & ~7U) != YMFPCI_OK))
        return false; /* fixed 48k, VRA/DRA/SPDIF off */
    /*
     * Restore the earlier 3D initialization order. The user reports the
     * audible effect disappeared after this block was moved below the route
     * and volume writes; keep all playback, capture, and PCI/DMA logic intact.
     */
    if (fThreeDWideSupported) {
        YMFWord current3D = 0xffff, currentGeneralPurpose = 0xffff;
        YMFWord expected3D = 0, expectedGeneralPurpose = 0;
        YMFWord readback3D = 0xffff, readbackGeneralPurpose = 0xffff;
        if (ymfCodecRead(io, YMF_AC97_3D_CONTROL_REGISTER, current3D) != YMFPCI_OK ||
            ymfCodecRead(io, YMF_AC97_GENERAL_PURPOSE_REGISTER,
                currentGeneralPurpose) != YMFPCI_OK ||
            !ymfAC97ThreeDWideRegisters(current3D, currentGeneralPurpose,
                fThreeDWide, expected3D, expectedGeneralPurpose) ||
            ymfCodecWrite(io, YMF_AC97_3D_CONTROL_REGISTER, expected3D) != YMFPCI_OK ||
            ymfCodecWrite(io, YMF_AC97_GENERAL_PURPOSE_REGISTER,
                expectedGeneralPurpose) != YMFPCI_OK ||
            ymfCodecRead(io, YMF_AC97_3D_CONTROL_REGISTER, readback3D) != YMFPCI_OK ||
            ymfCodecRead(io, YMF_AC97_GENERAL_PURPOSE_REGISTER,
                readbackGeneralPurpose) != YMFPCI_OK ||
            (readback3D & YMF_AC97_3D_READBACK_MASK) !=
                (fThreeDWide ? YMF_AC97_3D_READBACK_WIDE_VALUE : 0) ||
            (readbackGeneralPurpose & YMF_AC97_3D_ENABLE_MASK) !=
                (expectedGeneralPurpose & YMF_AC97_3D_ENABLE_MASK))
            return false;
        IOLog("YMFPCI: AC97 3D request=%04x readback=%04x GP=%04x; register state only, audible effect unverified\n",
            expected3D, readback3D, readbackGeneralPurpose);
    } else {
        IOLog("YMFPCI: 3D Wide control unavailable for AC97 codec %04x%04x\n",
            vendor1, vendor2);
    }
    /* Mute all playback/monitor sources except the microphone ADC input. */
    const unsigned muted[] = { 0x06, 0x0a, 0x10, 0x12, 0x14, 0x16 };
    for (unsigned i = 0; i < sizeof(muted) / sizeof(muted[0]); ++i)
        if (ymfCodecWrite(io, muted[i], 0x8000) != YMFPCI_OK)
            return false;
    /* Match the user's working YMF754 source: keep the AC97 mic monitor muted
     * while retaining its +20 dB boost; select Mic for ADC recording. */
    YMFWord micRegister = 0;
    if (!ymfAC97MicVolumeRegister(
            static_cast<unsigned>(fMicVolumeValue), micRegister) ||
        ymfCodecWrite(io, 0x1a, 0x0000) != YMFPCI_OK ||
        ymfCodecWrite(io, 0x0e, micRegister) != YMFPCI_OK ||
        ymfCodecWrite(io, 0x1c, 0x0000) != YMFPCI_OK)
        return false;
    YMFWord micVolume = 0xffff, recordSelect = 0xffff, recordGain = 0xffff;
    if (ymfCodecRead(io, 0x0e, micVolume) != YMFPCI_OK ||
        ymfCodecRead(io, 0x1a, recordSelect) != YMFPCI_OK ||
        ymfCodecRead(io, 0x1c, recordGain) != YMFPCI_OK) {
        IOLog("YMFPCI: AC97 microphone route readback unavailable\n");
    } else {
        IOLog("YMFPCI: AC97 microphone route mic-volume=%04x record-select=%04x record-gain=%04x\n",
            micVolume, recordSelect, recordGain);
    }
    /* Preserve the selected output level across sleep/wake. PCM stays unity. */
    YMFWord expectedMaster = 0;
    if (!ymfAC97MasterVolumeRegister(static_cast<unsigned>(fVolumeValue),
            expectedMaster, fOutputMuted) ||
        ymfCodecWrite(io, 0x18, 0) != YMFPCI_OK ||
        ymfCodecWrite(io, 0x02, expectedMaster) != YMFPCI_OK)
        return false;
    YMFWord master = 0, pcm = 0;
    if (ymfCodecRead(io, 0x02, master) != YMFPCI_OK ||
        ymfCodecRead(io, 0x18, pcm) != YMFPCI_OK ||
        (master & 0x9f1f) != expectedMaster || (pcm & 0x9f1f) != 0) {
        IOLog("YMFPCI: AC97 output readback failed: master=%04x pcm=%04x\n", master, pcm);
        return false;
    }
    IOLog("YMFPCI: AC97 vendor=%04x%04x; master=%04x pcm=%04x\n",
        vendor1, vendor2, master, pcm);
    return true;
}

IOReturn YMFPCIAudioEngine::volumeValueChanged(OSObject *target,
    IOAudioControl *, SInt32, SInt32 newValue)
{
    YMFPCIAudioEngine *engine = OSDynamicCast(YMFPCIAudioEngine, target);
    if (!engine || !engine->fDevice || !engine->fReady || engine->fSuspended)
        return kIOReturnNotReady;
    if (newValue < YMF_AC97_VOLUME_MIN || newValue > YMF_AC97_VOLUME_MAX)
        return kIOReturnBadArgument;
    YMFWord regValue = 0;
    if (!ymfAC97MasterVolumeRegister(static_cast<unsigned>(newValue), regValue,
            engine->fOutputMuted))
        return kIOReturnBadArgument;
    if (ymfCodecWrite(engine->fDevice->fHardwareIO, 0x02, regValue) != YMFPCI_OK)
        return kIOReturnIOError;
    engine->fVolumeValue = newValue;
    IOLog("YMFPCI: output volume %d/31; AC97 master=%04x\n",
        static_cast<int>(newValue), regValue);
    return kIOReturnSuccess;
}

IOReturn YMFPCIAudioEngine::muteValueChanged(OSObject *target,
    IOAudioControl *, SInt32, SInt32 newValue)
{
    YMFPCIAudioEngine *engine = OSDynamicCast(YMFPCIAudioEngine, target);
    if (!engine || !engine->fDevice || !engine->fReady || engine->fSuspended)
        return kIOReturnNotReady;
    if (newValue != 0 && newValue != 1)
        return kIOReturnBadArgument;

    const bool muted = newValue != 0;
    YMFWord regValue = 0;
    if (!ymfAC97MasterVolumeRegister(
            static_cast<unsigned>(engine->fVolumeValue), regValue, muted))
        return kIOReturnBadArgument;
    if (ymfCodecWrite(engine->fDevice->fHardwareIO, 0x02, regValue) != YMFPCI_OK)
        return kIOReturnIOError;
    engine->fOutputMuted = muted;
    IOLog("YMFPCI: output mute=%d; AC97 master=%04x\n",
        muted ? 1 : 0, regValue);
    return kIOReturnSuccess;
}

IOReturn YMFPCIAudioEngine::threeDWideValueChanged(OSObject *target,
    IOAudioControl *, SInt32, SInt32 newValue)
{
    YMFPCIAudioEngine *engine = OSDynamicCast(YMFPCIAudioEngine, target);
    if (!engine || !engine->fDevice || !engine->fReady ||
        engine->fDigitalOutput || !engine->fThreeDWideSupported ||
        engine->fSuspended)
        return kIOReturnNotReady;
    if (newValue != 0 && newValue != 1)
        return kIOReturnBadArgument;

    const bool enabled = newValue != 0;
    if (enabled == engine->fThreeDWide)
        return kIOReturnSuccess;

    const YMFPCIHardwareIO &io = engine->fDevice->fHardwareIO;
    YMFWord old3D = 0xffff, oldGeneralPurpose = 0xffff;
    YMFWord expected3D = 0, expectedGeneralPurpose = 0;
    if (ymfCodecRead(io, YMF_AC97_3D_CONTROL_REGISTER, old3D) != YMFPCI_OK ||
        ymfCodecRead(io, YMF_AC97_GENERAL_PURPOSE_REGISTER,
            oldGeneralPurpose) != YMFPCI_OK ||
        !ymfAC97ThreeDWideRegisters(old3D, oldGeneralPurpose, enabled,
            expected3D, expectedGeneralPurpose))
        return kIOReturnIOError;

    if (ymfCodecWrite(io, YMF_AC97_3D_CONTROL_REGISTER, expected3D) != YMFPCI_OK)
        return kIOReturnIOError;
    if (ymfCodecWrite(io, YMF_AC97_GENERAL_PURPOSE_REGISTER,
            expectedGeneralPurpose) != YMFPCI_OK) {
        ymfCodecWrite(io, YMF_AC97_3D_CONTROL_REGISTER, old3D);
        ymfCodecWrite(io, YMF_AC97_GENERAL_PURPOSE_REGISTER, oldGeneralPurpose);
        return kIOReturnIOError;
    }

    YMFWord readback3D = 0xffff, readbackGeneralPurpose = 0xffff;
    if (ymfCodecRead(io, YMF_AC97_3D_CONTROL_REGISTER, readback3D) != YMFPCI_OK ||
        ymfCodecRead(io, YMF_AC97_GENERAL_PURPOSE_REGISTER,
            readbackGeneralPurpose) != YMFPCI_OK ||
        (readback3D & YMF_AC97_3D_READBACK_MASK) !=
            (enabled ? YMF_AC97_3D_READBACK_WIDE_VALUE : 0) ||
        (readbackGeneralPurpose & YMF_AC97_3D_ENABLE_MASK) !=
            (expectedGeneralPurpose & YMF_AC97_3D_ENABLE_MASK)) {
        ymfCodecWrite(io, YMF_AC97_3D_CONTROL_REGISTER, old3D);
        ymfCodecWrite(io, YMF_AC97_GENERAL_PURPOSE_REGISTER, oldGeneralPurpose);
        return kIOReturnIOError;
    }

    engine->fThreeDWide = enabled;
    IOLog("YMFPCI: 3D Wide %s; AC97 3D=%04x GP=%04x\n",
        enabled ? "enabled" : "disabled", readback3D, readbackGeneralPurpose);
    return kIOReturnSuccess;
}

IOReturn YMFPCIAudioEngine::micVolumeValueChanged(OSObject *target,
    IOAudioControl *, SInt32, SInt32 newValue)
{
    YMFPCIAudioEngine *engine = OSDynamicCast(YMFPCIAudioEngine, target);
    if (!engine || !engine->fDevice || !engine->fReady || engine->fSuspended)
        return kIOReturnNotReady;
    if (newValue < YMF_AC97_MIC_VOLUME_MIN ||
        newValue > YMF_AC97_MIC_VOLUME_MAX)
        return kIOReturnBadArgument;
    YMFWord regValue = 0;
    if (!ymfAC97MicVolumeRegister(static_cast<unsigned>(newValue), regValue))
        return kIOReturnBadArgument;
    if (ymfCodecWrite(engine->fDevice->fHardwareIO, 0x0e, regValue) != YMFPCI_OK)
        return kIOReturnIOError;
    engine->fMicVolumeValue = newValue;
    IOLog("YMFPCI: microphone sensitivity %d/31; AC97 mic-volume=%04x\n",
        static_cast<int>(newValue), regValue);
    return kIOReturnSuccess;
}

void YMFPCIAudioEngine::initializePlaybackBanks()
{
    if (!fControl.bytes || !fSamples.bytes)
        return;
    for (unsigned voice = 0; voice < 2; ++voice)
        for (unsigned bank = 0; bank < 2; ++bank) {
            YMFPlaybackBank *p = reinterpret_cast<YMFPlaybackBank *>(
                static_cast<UInt8 *>(fControl.bytes) + fLayout.playbackOffset +
                (voice * 2 + bank) * fLayout.playbackStride);
            ymfPCMInitBank(*p, voice, fSamples.address);
            if (fDigitalOutput) {
                p->leftGain = p->leftGainEnd = 0;
                p->rightGain = p->rightGainEnd = 0;
                p->eff2Gain = p->eff2GainEnd = 0;
                p->eff3Gain = p->eff3GainEnd = 0;
                if (voice == 0)
                    p->eff3Gain = p->eff3GainEnd = 0x40000000U;
                else
                    p->eff2Gain = p->eff2GainEnd = 0x40000000U;
            }
        }
}

void YMFPCIAudioEngine::initializeBanks()
{
    bzero(fControl.bytes, fLayout.totalBytes);
    if (fCaptureSamples.bytes)
        bzero(fCaptureSamples.bytes, YMF_PCM_BYTES);
    initializePlaybackBanks();
    if (fCaptureSamples.bytes) {
        for (unsigned bank = 0; bank < 2; ++bank) {
            YMFPCaptureBank *capture = reinterpret_cast<YMFPCaptureBank *>(
                static_cast<UInt8 *>(fControl.bytes) + fLayout.captureOffset +
                (YMF_PCM_MIC_CAPTURE_VOICE * 2 + bank) * fLayout.captureStride);
            ymfPCMInitCaptureBank(*capture, fCaptureSamples.address, YMF_PCM_BYTES);
        }
    }
    static_cast<UInt32 *>(fControl.bytes)[0] = YMF_PCM_VOICES;
    dmaFence();
}

/* Prepare and publish the AC'97 ADC route while the DSP is stopped.
 *
 * The firmware latches MAPOFREC and the capture descriptors when MODE starts.
 * Enabling the record map later can read back as enabled but leave both bank
 * positions at zero, producing silent capture. The DSP only runs while an
 * analog playback or input client owns it; captured samples remain in the
 * driver's DMA ring and are delivered only through an active input stream. */
void YMFPCIAudioEngine::prepareCapturePath()
{
    if (!fDevice || !fCaptureSamples.bytes || !fControl.bytes ||
        fLayout.captureStride < sizeof(YMFPCaptureBank)) {
        fCaptureRunning = false;
        return;
    }

    const YMFPCIHardwareIO &io = fDevice->fHardwareIO;
    io.write32(io.context, YMF_REG_ADCSLOTSR, YMF_PCM_ADC_48K_SLOT_RATE);
    io.write32(io.context, YMF_REG_ADCFORMAT, YMF_PCM_CAPTURE_STEREO_FORMAT);
    for (unsigned bank = 0; bank < 2; ++bank) {
        YMFPCaptureBank *capture = reinterpret_cast<YMFPCaptureBank *>(
            static_cast<UInt8 *>(fControl.bytes) + fLayout.captureOffset +
            (YMF_PCM_MIC_CAPTURE_VOICE * 2 + bank) * fLayout.captureStride);
        ymfPCMInitCaptureBank(*capture, fCaptureSamples.address, YMF_PCM_BYTES);
    }
    dmaFence();
    io.write32(io.context, YMF_REG_MAPOFREC,
        1U << YMF_PCM_MIC_CAPTURE_VOICE);
    const UInt32 recordMap = io.read32(io.context, YMF_REG_MAPOFREC);
    fCaptureRunning = recordMap != 0xffffffffU &&
        (recordMap & (1U << YMF_PCM_MIC_CAPTURE_VOICE)) != 0;
    IOLog("YMFPCI: capture path published MAPOFREC=%08x format=%08x rate=%08x bank-base=%08x loop-end=%u\n",
        recordMap,
        io.read32(io.context, YMF_REG_ADCFORMAT),
        io.read32(io.context, YMF_REG_ADCSLOTSR), fCaptureSamples.address,
        static_cast<unsigned>(YMF_PCM_BYTES));
}

bool YMFPCIAudioEngine::configureHardware()
{
    const YMFPCIHardwareIO &io = fDevice->fHardwareIO;
    io.write32(io.context, YMF_REG_MODE, 0);
    if (ymfStopDSP(io) != YMFPCI_OK)
        return false;
    initializeBanks();
    io.write32(io.context, YMF_REG_PLAYCTRLBASE, fControl.address);
    io.write32(io.context, YMF_REG_RECCTRLBASE, fControl.address + fLayout.captureOffset);
    io.write32(io.context, YMF_REG_EFFCTRLBASE, fControl.address + fLayout.effectOffset);
    io.write32(io.context, YMF_REG_WORKBASE, fControl.address + fLayout.workOffset);
    io.write32(io.context, YMF_REG_WORKSIZE, YMF_PCM_WORK_BYTES / 4);
    io.write32(io.context, YMF_REG_MAPOFREC, 0);
    if (!fDigitalOutput)
        prepareCapturePath();
    else
        fCaptureRunning = false;
    io.write32(io.context, YMF_REG_MAPOFEFFECT, 0);
    if (fDigitalOutput) {
        io.write32(io.context, YMF_REG_MODE,
            io.read32(io.context, YMF_REG_MODE) | (1U << 30));
        const UInt16 secondary = io.read16(io.context, YMF_REG_SECCONFIG);
        io.write16(io.context, YMF_REG_SECCONFIG,
            static_cast<UInt16>((secondary & ~0x0330U) | 0x0010U));
    } else {
        const UInt16 secondary = io.read16(io.context, YMF_REG_SECCONFIG);
        io.write16(io.context, YMF_REG_SECCONFIG,
            static_cast<UInt16>(secondary & ~0x0010U));
    }
    io.write16(io.context, YMF_REG_TIMERCTRL, 0);
    io.write16(io.context, YMF_REG_SPDIFOUTCTRL, fDigitalOutput ? 2 : 0);
    io.write16(io.context, YMF_REG_SPDIFOUTSTATUS, YMF_SPDIF_DEFAULT_STATUS);
    io.write16(io.context, YMF_REG_SPDIFINCTRL, 0);
    for (unsigned offset = 0x80; offset < 0xc0; offset += 4)
        io.write32(io.context, offset, 0);
    io.write32(io.context, YMF_REG_NATIVEDACOUTVOL, 0x3fff3fffU);
    io.write32(io.context, YMF_REG_BUF441OUTVOL, 0x3fff3fff);
    io.write32(io.context, YMF_REG_ZVOUTVOL, 0x3fff3fff);
    io.write32(io.context, YMF_REG_SPDIFOUTVOL, 0x3fff3fff);
    io.write32(io.context, YMF_REG_LEGACYOUTVOL, 0x3fff3fff);
    /* Linux/YMFPCI path: full-scale native ADC input gain is required for
     * capture on the tested YMF754, independently of the AC97 ADC path. */
    io.write32(io.context, YMF_REG_NATIVEADCINVOL, 0x3fff3fff);
    io.write32(io.context, YMF_REG_NATIVEDACINVOL, 0x3fff3fff);
    /* Explicitly mute the AC97 ADC-to-playback path as well as the ADC loop
     * mixer. This does not disable the independent capture DMA route. */
    io.write32(io.context, YMF_REG_PRIADCOUTVOL, 0);
    io.write32(io.context, YMF_REG_PRIADCLOOPVOL, 0);
    /* ALSA clears the complete STATUS register during device teardown. Clear
     * any cold-boot latch as well as the IRQ bit before enabling the DSP. */
    io.write32(io.context, YMF_REG_STATUS, 0xffffffffU);
    io.write16(io.context, YMF_REG_INTFLAG, 0xffffU);
    io.write32(io.context, YMF_REG_CONFIG, 1);
    const UInt32 config = io.read32(io.context, YMF_REG_CONFIG);
    const UInt32 playbackBase = io.read32(io.context, YMF_REG_PLAYCTRLBASE);
    const UInt32 nativeVolume = io.read32(io.context, YMF_REG_NATIVEDACOUTVOL);
    if (config != 1 || playbackBase != fControl.address ||
        nativeVolume != 0x3fff3fff) {
        IOLog("YMFPCI: DSP setup readback failed: config=%08x playbase=%08x/%08x nativeout=%08x\n",
            config, playbackBase, fControl.address, nativeVolume);
        return false;
    }
    fDevice->fConfiguredAudioEngine = this;
    IOLog("YMFPCI: ADC path native-input=%08x playback=%08x loop=%08x secondary-playback=%08x secondary-loop=%08x\n",
        io.read32(io.context, YMF_REG_NATIVEADCINVOL),
        io.read32(io.context, YMF_REG_PRIADCOUTVOL),
        io.read32(io.context, YMF_REG_PRIADCLOOPVOL),
        io.read32(io.context, YMF_REG_SECADCOUTVOL),
        io.read32(io.context, YMF_REG_SECADCLOOPVOL));
    return true;
}

IOReturn YMFPCIAudioEngine::performAudioEngineStart()
{
    if (!fReady || fSuspended || !fDevice || !fLifecycleLock)
        return kIOReturnNotReady;

    /* The card has one DSP and one DMA register set. Stop capture and the
     * previously selected output before handing ownership to another output. */
    IOLockLock(fLifecycleLock);
    const bool playbackAlreadyActive = fPlaybackActive;
    IOLockUnlock(fLifecycleLock);
    if (playbackAlreadyActive)
        return kIOReturnSuccess;

    if (fDigitalOutput && fDevice->fMicEngine &&
        fDevice->fMicEngine->fRunning) {
        const IOReturn micStop = fDevice->fMicEngine->stopAudioEngine();
        if (micStop != kIOReturnSuccess)
            return micStop;
    }
    YMFPCIAudioEngine *other = fDigitalOutput ?
        fDevice->fAudioEngine : fDevice->fDigitalAudioEngine;
    if (other && other != this && other->fRunning) {
        const IOReturn stopResult = other->stopAudioEngine();
        if (stopResult != kIOReturnSuccess)
            return stopResult;
        IOLog("YMFPCI: stopped previous %s output before %s start\n",
            other->fDigitalOutput ? "S/PDIF" : "analog",
            fDigitalOutput ? "S/PDIF" : "analog");
    }

    IOLockLock(fLifecycleLock);
    if (fPlaybackActive) {
        IOLockUnlock(fLifecycleLock);
        return kIOReturnSuccess;
    }
    fPlaybackActive = true;
    IOReturn result = kIOReturnSuccess;
    if (!fRunning) {
        result = startDSP();
    } else {
        initializePlaybackBanks();
        bzero(fSamples.bytes, YMF_PCM_BYTES);
        dmaFence();
        const IOInterruptState state =
            IOSimpleLockLockDisableInterrupt(fLock);
        UInt32 *controlHeader = static_cast<UInt32 *>(fControl.bytes);
        controlHeader[1] = fControl.address + fLayout.playbackOffset;
        controlHeader[2] = fControl.address + fLayout.playbackOffset +
            2 * fLayout.playbackStride;
        fDevice->fHardwareIO.write32(fDevice->fHardwareIO.context,
            YMF_REG_NATIVEDACOUTVOL, 0x3fff3fffU);
        dmaFence();
        fLastFrame = 0;
        takeTimeStamp(false);
        IOSimpleLockUnlockEnableInterrupt(fLock, state);
        IOLog("YMFPCI: playback joined active capture DSP; analog DAC enabled\n");
    }
    if (result != kIOReturnSuccess)
        fPlaybackActive = false;
    IOLockUnlock(fLifecycleLock);
    return result;
}

IOReturn YMFPCIAudioEngine::startCaptureEngine()
{
    if (!fReady || fSuspended || !fDevice || !fLifecycleLock)
        return kIOReturnNotReady;
    if (fDevice->fDigitalAudioEngine &&
        fDevice->fDigitalAudioEngine->fRunning)
        return kIOReturnNotReady;
    IOLockLock(fLifecycleLock);
    if (fCaptureActive) {
        IOLockUnlock(fLifecycleLock);
        return kIOReturnSuccess;
    }
    fCaptureActive = true;
    IOReturn result = kIOReturnSuccess;
    if (!fRunning)
        result = startDSP();
    else
        updateCaptureRouting();
    if (result != kIOReturnSuccess)
        fCaptureActive = false;
    IOLockUnlock(fLifecycleLock);
    return result;
}

IOReturn YMFPCIAudioEngine::stopCaptureEngine()
{
    if (!fLifecycleLock)
        return kIOReturnSuccess;
    IOLockLock(fLifecycleLock);
    if (!fCaptureActive) {
        IOLockUnlock(fLifecycleLock);
        return kIOReturnSuccess;
    }
    fCaptureActive = false;
    if (fRunning)
        updateCaptureRouting();
    const IOReturn result = fPlaybackActive ? kIOReturnSuccess : stopDSP();
    IOLockUnlock(fLifecycleLock);
    return result;
}

IOReturn YMFPCIAudioEngine::startDSP()
{
    if (!fReady || fSuspended || !fDevice ||
        !fDevice->fInterruptSource || !fDevice->fInterruptSourceAdded)
        return kIOReturnNotReady;
    if (fRunning)
        return kIOReturnSuccess;

    if (fDevice->fConfiguredAudioEngine != this && !configureHardware()) {
        IOLog("YMFPCI: shared DSP reconfiguration failed\n");
        return kIOReturnIOError;
    }
    const YMFPCIHardwareIO &io = fDevice->fHardwareIO;
    const UInt32 stoppedMode = io.read32(io.context, YMF_REG_MODE);
    if (stoppedMode == 0xffffffffU || (stoppedMode & 3U)) {
        IOLog("YMFPCI: refusing PCM start; DSP MODE is not stopped (%08x)\n",
            stoppedMode);
        return kIOReturnIOError;
    }
    /* Shared control memory is initialized only while MODE is stopped. The
     * microphone engine can then join/leave without resetting live playback
     * banks or the capture ring. */
    initializeBanks();
    if (!fDigitalOutput)
        prepareCapturePath();
    UInt32 *controlHeader = static_cast<UInt32 *>(fControl.bytes);
    const UInt8 *bankData = static_cast<const UInt8 *>(fControl.bytes) +
        fLayout.playbackOffset;
    for (unsigned voice = 0; voice < 2; ++voice) {
        for (unsigned bankIndex = 0; bankIndex < 2; ++bankIndex) {
            const YMFPlaybackBank *bank =
                reinterpret_cast<const YMFPlaybackBank *>(bankData +
                    (voice * 2 + bankIndex) * fLayout.playbackStride);
            IOLog("YMFPCI: playback-bank v=%u.%u fmt=%08x default=%08x base=%08x loop=%08x/%08x frac=%08x end=%08x/%08x/%08x/%08x/%08x fx=%08x/%08x/%08x q=%08x\n",
                voice, bankIndex, bank->format, bank->loopDefault, bank->base,
                bank->loopStart, bank->loopEnd, bank->loopFrac,
                bank->deltaEnd, bank->lpfKEnd, bank->egGainEnd,
                bank->leftGainEnd, bank->rightGainEnd, bank->eff1GainEnd,
                bank->eff2GainEnd, bank->eff3GainEnd, bank->lpfQ);
            IOLog("YMFPCI: playback-bank state v=%u.%u status=%08x frames=%08x count=%08x start=%08x+%08x delta=%08x lpfK=%08x eg=%08x LR=%08x/%08x fx=%08x/%08x/%08x lpfD=%08x/%08x\n",
                voice, bankIndex, bank->status, bank->numOfFrames,
                bank->loopCount, bank->start, bank->startFrac, bank->delta,
                bank->lpfK, bank->egGain, bank->leftGain, bank->rightGain,
                bank->eff1Gain, bank->eff2Gain, bank->eff3Gain,
                bank->lpfD1, bank->lpfD2);
        }
    }
    const UInt32 initialDACVolume = fPlaybackActive ? 0x3fff3fffU : 0;
    io.write32(io.context, YMF_REG_NATIVEDACOUTVOL, initialDACVolume);
    /* A cold-start log showed STATUS=0x00008000 here, unlike the working
     * Windows-initialized path (STATUS=0). Clear all latched status before DMA. */
    io.write32(io.context, YMF_REG_STATUS, 0xffffffffU);
    io.read32(io.context, YMF_REG_STATUS);
    io.write16(io.context, YMF_REG_INTFLAG, 0xffffU);
    io.read16(io.context, YMF_REG_INTFLAG);
    const IOInterruptState state = IOSimpleLockLockDisableInterrupt(fLock);
    fLastFrame = 0;
    fReportedIRQ = false;
    fReportedProgress = fReportedWrap = fReportedZeroFrame = fReportedPCMData = false;
    fReportedOtherIRQ = false;
    fReportedDSPState = false;
    fDiagnosticFrames = fPendingDiagnostics = 0;
    fPendingIRQStatus = 0;
    fPendingINTFlag = 0;
    takeTimeStamp(false);
    fDevice->fPCIDevice->setBusMasterEnable(true);
    fRunning = true;
    const UInt16 command = fDevice->fPCIDevice->configRead16(kIOPCIConfigCommand);
    if (command == 0xffff || !(command & 4)) {
        IOSimpleLockUnlockEnableInterrupt(fLock, state);
        stopDSP();
        return kIOReturnIOError;
    }
    io.write32(io.context, YMF_REG_MODE, stoppedMode | 3U);
    const UInt32 mode = io.read32(io.context, YMF_REG_MODE);
    if (mode == 0xffffffffU || (mode & 3) != 3) {
        IOSimpleLockUnlockEnableInterrupt(fLock, state);
        stopDSP();
        return kIOReturnIOError;
    }
    if (fPlaybackActive) {
        controlHeader[1] = fControl.address + fLayout.playbackOffset;
        controlHeader[2] = fControl.address + fLayout.playbackOffset +
            2 * fLayout.playbackStride;
    } else {
        controlHeader[1] = 0;
        controlHeader[2] = 0;
    }
    dmaFence();
    const UInt32 status = io.read32(io.context, YMF_REG_STATUS);
    const UInt32 selector = io.read32(io.context, YMF_REG_CTRLSELECT);
    const UInt32 frame = currentFrameLocked();
    const UInt32 nativeVolume = io.read32(io.context, YMF_REG_NATIVEDACOUTVOL);
    const UInt32 voiceCount = controlHeader[0];
    const UInt32 voice0BankPointer = controlHeader[1];
    const UInt32 voice1BankPointer = controlHeader[2];
    const UInt8 *controlBytes = static_cast<const UInt8 *>(fControl.bytes);
    const YMFPlaybackBank *voice0Bank =
        reinterpret_cast<const YMFPlaybackBank *>(controlBytes + fLayout.playbackOffset);
    const YMFPlaybackBank *voice1Bank =
        reinterpret_cast<const YMFPlaybackBank *>(controlBytes + fLayout.playbackOffset +
            2 * fLayout.playbackStride);
    const UInt32 voice0Base = voice0Bank->base;
    const UInt32 voice1Base = voice1Bank->base;
    const UInt32 voice0Format = voice0Bank->format;
    const UInt32 voice1Format = voice1Bank->format;
    const UInt32 loopEnd = voice0Bank->loopEnd;
    IOSimpleLockUnlockEnableInterrupt(fLock, state);
    if (fCaptureActive)
        updateCaptureRouting();
    IOLog("YMFPCI: shared DSP state pci=%04x config=%08x mode=%08x status=%08x bank=%u frame=%u nativeout=%08x playback=%u capture=%u\n",
        command, io.read32(io.context, YMF_REG_CONFIG), mode, status,
        selector & 1, frame, nativeVolume, fPlaybackActive ? 1U : 0U,
        fCaptureActive ? 1U : 0U);
    IOLog("YMFPCI: DMA map control=%08x samples=%08x voices=%u voice-pointers=%08x/%08x bankbase=%08x/%08x format=%08x/%08x loop=%u\n",
        fControl.address, fSamples.address, voiceCount, voice0BankPointer, voice1BankPointer,
        voice0Base, voice1Base, voice0Format, voice1Format, loopEnd);
    IOLog("YMFPCI: shared DSP started; waiting for hardware IRQ\n");
    return kIOReturnSuccess;
}

IOReturn YMFPCIAudioEngine::performAudioEngineStop()
{
    if (!fLifecycleLock)
        return kIOReturnSuccess;
    IOLockLock(fLifecycleLock);
    if (!fPlaybackActive) {
        IOLockUnlock(fLifecycleLock);
        return kIOReturnSuccess;
    }
    fPlaybackActive = false;
    if (fRunning) {
        const IOInterruptState state = IOSimpleLockLockDisableInterrupt(fLock);
        const YMFPCIHardwareIO &io = fDevice->fHardwareIO;
        io.write32(io.context, YMF_REG_NATIVEDACOUTVOL, 0);
        UInt32 *controlHeader = static_cast<UInt32 *>(fControl.bytes);
        controlHeader[1] = 0;
        controlHeader[2] = 0;
        dmaFence();
        IOSimpleLockUnlockEnableInterrupt(fLock, state);
    }
    const IOReturn result = fCaptureActive ? kIOReturnSuccess : stopDSP();
    if (fCaptureActive)
        IOLog("YMFPCI: playback stopped; microphone DSP remains active\n");
    IOLockUnlock(fLifecycleLock);
    return result;
}

IOReturn YMFPCIAudioEngine::stopDSP()
{
    if (!fDevice || !fLock)
        return kIOReturnSuccess;
    if (!fRunning)
        return kIOReturnSuccess;
    const IOInterruptState state = IOSimpleLockLockDisableInterrupt(fLock);
    fRunning = false;
    const YMFPCIHardwareIO &io = fDevice->fHardwareIO;
    const UInt32 recordMap = io.read32(io.context, YMF_REG_MAPOFREC);
    if (recordMap != 0xffffffffU)
        io.write32(io.context, YMF_REG_MAPOFREC,
            recordMap & ~(1U << YMF_PCM_MIC_CAPTURE_VOICE));
    fCaptureRunning = false;
    io.write32(io.context, YMF_REG_NATIVEDACOUTVOL, 0);
    const UInt32 mode = io.read32(io.context, YMF_REG_MODE);
    if (mode != 0xffffffffU)
        io.write32(io.context, YMF_REG_MODE, mode & ~3U);
    IOSimpleLockUnlockEnableInterrupt(fLock, state);
    /* Once fRunning is false, the filter never touches the registers/banks.
     * Keep the bounded DSP wait out of the spin lock / IRQ-disabled section. */
    const YMFPCIResult result = ymfStopPCM(io);
    if (fDigitalOutput) {
        io.write16(io.context, YMF_REG_SPDIFOUTCTRL, 0);
        const UInt16 secondary = io.read16(io.context, YMF_REG_SECCONFIG);
        io.write16(io.context, YMF_REG_SECCONFIG,
            static_cast<UInt16>(secondary & ~0x0010U));
        const UInt32 modeAfterStop = io.read32(io.context, YMF_REG_MODE);
        if (modeAfterStop != 0xffffffffU)
            io.write32(io.context, YMF_REG_MODE, modeAfterStop & ~(1U << 30));
        fDevice->fConfiguredAudioEngine = 0;
    }
    fDevice->fPCIDevice->setBusMasterEnable(false);
    fDevice->fPCIDevice->configRead16(kIOPCIConfigCommand); /* flush */
    const IOInterruptState finalState = IOSimpleLockLockDisableInterrupt(fLock);
    io.write32(io.context, YMF_REG_STATUS, 0x80000000);
    if (fControl.bytes) {
        UInt32 *controlHeader = static_cast<UInt32 *>(fControl.bytes);
        controlHeader[1] = 0;
        controlHeader[2] = 0;
        dmaFence();
    }
    if (result != YMFPCI_OK)
        fReady = false; /* refuse restart until the engine is safely idle */
    IOSimpleLockUnlockEnableInterrupt(fLock, finalState);
    if (result != YMFPCI_OK)
        IOLog("YMFPCI: PCM stop failed: %s; PCI DMA disabled\n", ymfResultName(result));
    else
        IOLog("YMFPCI: PCM stopped; DSP idle and ready for next client\n");
    return result == YMFPCI_OK ? kIOReturnSuccess : kIOReturnIOError;
}

void YMFPCIAudioEngine::updateCaptureRouting()
{
    if (!fDevice || !fCaptureSamples.bytes || !fLock)
        return;
    const bool requested = fCaptureActive;
    const IOInterruptState state = IOSimpleLockLockDisableInterrupt(fLock);
    if (fRunning) {
        const YMFPCIHardwareIO &io = fDevice->fHardwareIO;
        if (requested) {
            /* Keep the AC'97 monitor muted; capture DMA itself was published
             * before MODE started and must not be switched live. */
            io.write32(io.context, YMF_REG_PRIADCOUTVOL, 0);
            io.write32(io.context, YMF_REG_PRIADCLOOPVOL, 0);
            IOLog("YMFPCI: ADC monitor at capture start playback=%08x loop=%08x\n",
                io.read32(io.context, YMF_REG_PRIADCOUTVOL),
                io.read32(io.context, YMF_REG_PRIADCLOOPVOL));
        }
        const UInt32 recordMap = io.read32(io.context, YMF_REG_MAPOFREC);
        const UInt32 mask = 1U << YMF_PCM_MIC_CAPTURE_VOICE;
        const bool enabled = recordMap != 0xffffffffU &&
            (recordMap & mask) != 0;
        if (!enabled && fCaptureRunning)
            IOLog("YMFPCI: capture route lost after DSP start; refusing live MAPOFREC rewrite\n");
        fCaptureRunning = enabled;
    }
    IOSimpleLockUnlockEnableInterrupt(fLock, state);
}

UInt32 YMFPCIAudioEngine::currentFrameLocked()
{
    if (!fRunning || !fControl.bytes || !fDevice)
        return 0;
    const YMFPCIHardwareIO &io = fDevice->fHardwareIO;
    const UInt32 active = io.read32(io.context, YMF_REG_CTRLSELECT);
    if (active == 0xffffffffU)
        return fLastFrame;
    dmaFence();
    volatile YMFPlaybackBank *bank = reinterpret_cast<volatile YMFPlaybackBank *>(
        static_cast<UInt8 *>(fControl.bytes) + fLayout.playbackOffset +
        (active & 1) * fLayout.playbackStride);
    const UInt32 frame = bank->start;
    return frame < YMF_PCM_FRAMES ? frame : fLastFrame;
}

UInt32 YMFPCIAudioEngine::currentCaptureFrame()
{
    if (!fLock)
        return 0;
    const IOInterruptState state = IOSimpleLockLockDisableInterrupt(fLock);
    UInt32 frame = 0;
    if (fRunning && fCaptureRunning && fControl.bytes && fDevice) {
        const YMFPCIHardwareIO &io = fDevice->fHardwareIO;
        const UInt32 active = io.read32(io.context, YMF_REG_CTRLSELECT);
        if (active != 0xffffffffU) {
            volatile YMFPCaptureBank *bank =
                reinterpret_cast<volatile YMFPCaptureBank *>(
                    static_cast<UInt8 *>(fControl.bytes) +
                    fLayout.captureOffset +
                    (YMF_PCM_MIC_CAPTURE_VOICE * 2 + (active & 1U)) *
                        fLayout.captureStride);
            dmaFence();
            const UInt32 bytePosition = bank->start;
            if (bytePosition != 0xffffffffU)
                frame = (bytePosition / 4U) % YMF_PCM_FRAMES;
        }
    }
    IOSimpleLockUnlockEnableInterrupt(fLock, state);
    return frame;
}

UInt32 YMFPCIAudioEngine::getCurrentSampleFrame()
{
    if (!fLock)
        return 0;
    const IOInterruptState state = IOSimpleLockLockDisableInterrupt(fLock);
    const UInt32 frame = currentFrameLocked();
    IOSimpleLockUnlockEnableInterrupt(fLock, state);
    return frame;
}

bool YMFPCIAudioEngine::interruptFilter(OSObject *owner, IOFilterInterruptEventSource *)
{
    YMFPCIAudioEngine *engine = static_cast<YMFPCIAudioEngine *>(owner);
    if (!engine || !engine->fDevice)
        return false;

    /* Keep the primary interrupt filter short: only identify this device's
     * shared PCI interrupt here. Acknowledge registers and update timestamps
     * later on the IOAudio workloop, matching the tested DS-XG architecture. */
    const YMFPCIHardwareIO &io = engine->fDevice->fHardwareIO;
    const UInt32 status = io.read32(io.context, YMF_REG_STATUS);
    const UInt16 intFlag = io.read16(io.context, YMF_REG_INTFLAG);
    return (status != 0xffffffffU && (status & 0x80000000U) != 0) ||
        (intFlag != 0 && intFlag != 0xffff);
}

void YMFPCIAudioEngine::interruptAction(OSObject *owner, IOInterruptEventSource *, int)
{
    YMFPCIAudioEngine *engine = static_cast<YMFPCIAudioEngine *>(owner);
    if (!engine || !engine->fLock)
        return;

    const IOInterruptState state = IOSimpleLockLockDisableInterrupt(engine->fLock);
    UInt32 diagnostics = 0;
    UInt32 frame = engine->fLastFrame;
    UInt32 interruptStatus = 0xffffffffU;
    UInt16 interruptFlags = 0;
    if (engine->fDevice) {
        const YMFPCIHardwareIO &io = engine->fDevice->fHardwareIO;
        interruptStatus = io.read32(io.context, YMF_REG_STATUS);
        interruptFlags = io.read16(io.context, YMF_REG_INTFLAG);
        const bool pcmInterrupt =
            interruptStatus != 0xffffffffU && (interruptStatus & 0x80000000U);
        const bool deviceInterrupt =
            interruptFlags != 0 && interruptFlags != 0xffff;

        /* One shared-IRQ action owns all YMF status acknowledgements. */
        if (deviceInterrupt) {
            io.write16(io.context, YMF_REG_INTFLAG, interruptFlags);
            io.read16(io.context, YMF_REG_INTFLAG);
            if (!pcmInterrupt && !engine->fReportedOtherIRQ) {
                engine->fPendingIRQStatus = interruptStatus;
                engine->fPendingINTFlag = interruptFlags;
                engine->fPendingDiagnostics |= 16;
                engine->fReportedOtherIRQ = true;
            }
        }

        if (pcmInterrupt) {
            if (engine->fRunning) {
                frame = engine->currentFrameLocked();
                if (!frame && !engine->fReportedZeroFrame) {
                    engine->fPendingDiagnostics |= 8;
                    engine->fReportedZeroFrame = true;
                }
                if (frame < engine->fLastFrame) {
                    engine->takeTimeStamp();
                    if (!engine->fReportedWrap) {
                        engine->fPendingDiagnostics |= 4;
                        engine->fReportedWrap = true;
                    }
                }
                engine->fLastFrame = frame;
                if (!engine->fReportedIRQ)
                    engine->fPendingDiagnostics |= 1;
                engine->fReportedIRQ = true;
                if (frame && !engine->fReportedProgress) {
                    engine->fPendingDiagnostics |= 2;
                    engine->fReportedProgress = true;
                }
            }
            io.write32(io.context, YMF_REG_STATUS, 0x80000000U);
            /* ALSA re-arms the bank interrupt through MODE bit 1. */
            if (engine->fRunning)
                io.write32(io.context, YMF_REG_MODE,
                    io.read32(io.context, YMF_REG_MODE) | 2U);
            io.read32(io.context, YMF_REG_STATUS);
        }
    }

    diagnostics = engine->fPendingDiagnostics;
    frame = engine->fLastFrame;
    interruptStatus = engine->fPendingIRQStatus;
    interruptFlags = engine->fPendingINTFlag;
    engine->fPendingDiagnostics = 0;
    IOSimpleLockUnlockEnableInterrupt(engine->fLock, state);
    if (engine->fDevice && engine->fDevice->fMicEngine)
        engine->fDevice->fMicEngine->updateTimestamp();
    if (diagnostics & 1)
        IOLog("YMFPCI: first PCM hardware IRQ received\n");
    if (diagnostics & 2)
        IOLog("YMFPCI: DMA position advancing (frame %u)\n", frame);
    if (diagnostics & 4)
        IOLog("YMFPCI: PCM ring wrap observed; CoreAudio timestamp updated\n");
    if (diagnostics & 8)
        IOLog("YMFPCI: IRQ arrived while DMA frame position was still zero\n");
    if (diagnostics & 16)
        IOLog("YMFPCI: non-PCM interrupt acknowledged: status=%08x intflag=%04x\n",
            interruptStatus, interruptFlags);
}

IOReturn YMFPCIAudioEngine::performFormatChange(IOAudioStream *,
    const IOAudioStreamFormat *format, const IOAudioSampleRate *rate)
{
    return supportedFormat(format) &&
        (!rate || (rate->whole == YMF_PCM_RATE && rate->fraction == 0)) ?
        kIOReturnSuccess : kIOReturnUnsupported;
}

IOReturn YMFPCIAudioEngine::clipOutputSamples(const void *mix, void *samples,
    UInt32 firstFrame, UInt32 frames, const IOAudioStreamFormat *format, IOAudioStream *)
{
    if (!mix || !samples || !format || !supportedFormat(format) ||
        firstFrame >= YMF_PCM_FRAMES || frames > YMF_PCM_FRAMES - firstFrame)
        return kIOReturnBadArgument;
    const UInt8 *source = static_cast<const UInt8 *>(mix) + firstFrame * 8;
    SInt16 *destination = static_cast<SInt16 *>(samples) + firstFrame * 2;
    UInt32 nonzeroSamples = 0;
    for (UInt32 i = 0; i < frames * 2; ++i) {
        UInt32 bits;
        memcpy(&bits, source + i * 4, sizeof(bits)); /* no aliasing/FPU */
        destination[i] = ymfPCMClipFloatBits(bits);
        if (destination[i] != 0)
            ++nonzeroSamples;
    }
    dmaFence();
    if (fRunning && !fReportedPCMData) {
        fReportedPCMData = true;
        IOLog("YMFPCI: CoreAudio PCM buffer frames=%u nonzero_samples=%u/%u\n",
            frames, nonzeroSamples, frames * 2);
    }
    if (fRunning && !fReportedDSPState) {
        fDiagnosticFrames += frames;
        if (fDiagnosticFrames >= YMF_PCM_RATE / 4) {
            const YMFPCIHardwareIO &io = fDevice->fHardwareIO;
            const UInt32 hardwareStatus = io.read32(io.context, YMF_REG_STATUS);
            const UInt32 mode = io.read32(io.context, YMF_REG_MODE);
            const UInt32 selector = io.read32(io.context, YMF_REG_CTRLSELECT);
            const UInt32 sampleCount = io.read32(io.context, YMF_REG_SAMPLECOUNT);
            const UInt32 numberOfSamples = io.read32(io.context, YMF_REG_NUMOFSAMPLES);
            UInt8 *controlBytes = static_cast<UInt8 *>(fControl.bytes);
            volatile YMFPlaybackBank *bank0 =
                reinterpret_cast<volatile YMFPlaybackBank *>(
                    controlBytes + fLayout.playbackOffset);
            volatile YMFPlaybackBank *bank1 =
                reinterpret_cast<volatile YMFPlaybackBank *>(
                    controlBytes + fLayout.playbackOffset + fLayout.playbackStride);
            volatile YMFPlaybackBank *bank2 =
                reinterpret_cast<volatile YMFPlaybackBank *>(
                    controlBytes + fLayout.playbackOffset + 2 * fLayout.playbackStride);
            volatile YMFPlaybackBank *bank3 =
                reinterpret_cast<volatile YMFPlaybackBank *>(
                    controlBytes + fLayout.playbackOffset + 3 * fLayout.playbackStride);
            dmaFence();
            const UInt32 start0 = bank0->start;
            const UInt32 start1 = bank1->start;
            const UInt32 start2 = bank2->start;
            const UInt32 start3 = bank3->start;
            const UInt32 bankStatus0 = bank0->status;
            const UInt32 bankStatus1 = bank1->status;
            const UInt32 bankStatus2 = bank2->status;
            const UInt32 bankStatus3 = bank3->status;
            fReportedDSPState = true;
            IOLog("YMFPCI: delayed DMA state status=%08x mode=%08x select=%08x samplecount=%08x numsamples=%08x bank-start=%u/%u/%u/%u bank-status=%08x/%08x/%08x/%08x\n",
                hardwareStatus, mode, selector, sampleCount, numberOfSamples,
                start0, start1, start2, start3, bankStatus0, bankStatus1,
                bankStatus2, bankStatus3);
        }
    }
    return kIOReturnSuccess;
}

IOReturn YMFPCIAudioEngine::convertInputSamples(const void *samples, void *mix,
    UInt32 firstFrame, UInt32 frames, const IOAudioStreamFormat *format,
    IOAudioStream *stream)
{
    if (!samples || !mix || !format ||
        !fCaptureSamples.bytes || !supportedFormat(format) ||
        firstFrame >= YMF_PCM_FRAMES || frames > YMF_PCM_FRAMES)
        return kIOReturnBadArgument;
    (void)stream;

    const IOInterruptState state = IOSimpleLockLockDisableInterrupt(fLock);
    const bool captureRunning = fRunning && fCaptureRunning;
    IOSimpleLockUnlockEnableInterrupt(fLock, state);
    if (!captureRunning) {
        bzero(mix, frames * 8);
        return kIOReturnSuccess;
    }

    dmaFence(); /* make DSP-written capture samples visible before CPU reads */
    /* Read from the YMF DMA ring directly, as in the previously verified
     * capture path. The IOAudioFamily input callback's source pointer can be a
     * callback window rather than the start of the hardware ring. */
    const SInt16 *source = static_cast<const SInt16 *>(fCaptureSamples.bytes);
    UInt32 *destination = static_cast<UInt32 *>(mix);
    UInt32 nonzeroSamples = 0;
    UInt32 peak = 0;
    UInt32 sourceFrame = firstFrame;
    for (UInt32 frame = 0; frame < frames; ++frame) {
        for (UInt32 channel = 0; channel < 2; ++channel) {
            const SInt16 sample = source[sourceFrame * 2 + channel];
            destination[frame * 2 + channel] = ymfPCMS16ToFloatBits(sample);
            if (sample)
                ++nonzeroSamples;
            const UInt32 magnitude = sample == -32768 ? 32768U :
                static_cast<UInt32>(sample < 0 ? -sample : sample);
            if (magnitude > peak)
                peak = magnitude;
        }
        if (++sourceFrame == YMF_PCM_FRAMES)
            sourceFrame = 0;
    }
    dmaFence();

    fCaptureDiagnosticFrames += frames;
    fCaptureDiagnosticNonzero += nonzeroSamples;
    if (peak > fCaptureDiagnosticPeak)
        fCaptureDiagnosticPeak = peak;
    if (fCaptureDiagnosticFrames >= YMF_PCM_RATE) {
        const UInt32 selector = fDevice->fHardwareIO.read32(
            fDevice->fHardwareIO.context, YMF_REG_CTRLSELECT);
        UInt32 bank0Position = 0xffffffffU;
        UInt32 bank1Position = 0xffffffffU;
        UInt32 bank0Base = 0xffffffffU;
        UInt32 bank1Base = 0xffffffffU;
        UInt32 bank0LoopEnd = 0xffffffffU;
        UInt32 bank1LoopEnd = 0xffffffffU;
        if (selector != 0xffffffffU) {
            const UInt8 *control = static_cast<const UInt8 *>(fControl.bytes);
            const YMFPCaptureBank *bank0 =
                reinterpret_cast<const YMFPCaptureBank *>(control +
                    fLayout.captureOffset +
                    (YMF_PCM_MIC_CAPTURE_VOICE * 2) * fLayout.captureStride);
            const YMFPCaptureBank *bank1 =
                reinterpret_cast<const YMFPCaptureBank *>(control +
                    fLayout.captureOffset +
                    (YMF_PCM_MIC_CAPTURE_VOICE * 2 + 1) * fLayout.captureStride);
            dmaFence();
            bank0Position = bank0->start;
            bank1Position = bank1->start;
            bank0Base = bank0->base;
            bank1Base = bank1->base;
            bank0LoopEnd = bank0->loopEnd;
            bank1LoopEnd = bank1->loopEnd;
        }
        IOLog("YMFPCI: microphone capture window frames=%u nonzero_samples=%u/%u peak=%u map=%08x format=%08x rate=%08x recbase=%08x/%08x bank-base=%08x/%08x loop-end=%u/%u bank-start=%u/%u\n",
            fCaptureDiagnosticFrames, fCaptureDiagnosticNonzero,
            fCaptureDiagnosticFrames * 2, fCaptureDiagnosticPeak,
            fDevice->fHardwareIO.read32(fDevice->fHardwareIO.context,
                YMF_REG_MAPOFREC),
            fDevice->fHardwareIO.read32(fDevice->fHardwareIO.context,
                YMF_REG_ADCFORMAT),
            fDevice->fHardwareIO.read32(fDevice->fHardwareIO.context,
                YMF_REG_ADCSLOTSR),
            fDevice->fHardwareIO.read32(fDevice->fHardwareIO.context,
                YMF_REG_RECCTRLBASE),
            fControl.address + fLayout.captureOffset,
            bank0Base, bank1Base, bank0LoopEnd, bank1LoopEnd,
            bank0Position, bank1Position);
        fCaptureDiagnosticFrames = 0;
        fCaptureDiagnosticNonzero = 0;
        fCaptureDiagnosticPeak = 0;
    }
    return kIOReturnSuccess;
}

void YMFPCIAudioEngine::suspendHardware()
{
    if (!fDevice || fSuspended)
        return;
    if (fLifecycleLock)
        IOLockLock(fLifecycleLock);
    stopDSP();
    fSuspended = true;
    if (fLifecycleLock)
        IOLockUnlock(fLifecycleLock);
}

bool YMFPCIAudioEngine::resumeHardware()
{
    if (!fSuspended)
        return fReady;
    if (!fDevice)
        return false;
    if (fLifecycleLock)
        IOLockLock(fLifecycleLock);
    if (fDigitalOutput && !fPlaybackActive && !fCaptureActive) {
        /* The analog engine restored firmware and owns the idle IRQ source.
         * No digital hardware setup is needed until S/PDIF is selected again. */
        fSuspended = false;
        fReady = true;
        if (fLifecycleLock)
            IOLockUnlock(fLifecycleLock);
        return true;
    }
    fDevice->fPCIDevice->setBusMasterEnable(false);
    fDevice->fPCIDevice->setMemoryEnable(true);
    const YMFPCIResult result = ymfUploadFirmware(fDevice->fHardwareIO,
        gYMFDSPFirmware, gYMFDSPFirmwareSize, gYMFControllerFirmware, gYMFControllerFirmwareSize);
    YMFPCMLayout layout;
    fReady = result == YMFPCI_OK && discoverLayout(layout) &&
        layout.playbackStride == fLayout.playbackStride &&
        layout.captureStride == fLayout.captureStride &&
        layout.effectStride == fLayout.effectStride &&
        (fDigitalOutput || configureCodec()) && configureHardware();
    if (fReady) {
        bzero(fSamples.bytes, YMF_PCM_BYTES);
        if (fCaptureSamples.bytes)
            bzero(fCaptureSamples.bytes, YMF_PCM_BYTES);
        fSuspended = false;
        if (fPlaybackActive || fCaptureActive)
            fReady = startDSP() == kIOReturnSuccess;
    } else {
        stopDSP();
    }
    if (!fReady)
        IOLog("YMFPCI: resume failed; audio and DMA remain disabled\n");
    if (fLifecycleLock)
        IOLockUnlock(fLifecycleLock);
    return fReady;
}

void YMFPCIAudioEngine::shutdown()
{
    /* Device termination can arrive outside the engine's command gate.
     * Serialize with CoreAudio start/format/power callbacks so a new client
     * cannot restart DMA between our stop and MMIO teardown. */
    IOCommandGate *gate = getCommandGate();
    IOLog("YMFPCI: engine shutdown requested digital=%u gate=%p\n",
        fDigitalOutput ? 1U : 0U, static_cast<void *>(gate));
    if (gate) {
        const IOReturn result = gate->runAction(shutdownAction);
        IOLog("YMFPCI: engine shutdown gate returned digital=%u result=%08x\n",
            fDigitalOutput ? 1U : 0U, static_cast<unsigned>(result));
    } else {
        IOLog("YMFPCI: engine shutdown running inline; no command gate\n");
        shutdownLocked();
        IOLog("YMFPCI: engine shutdown inline complete digital=%u\n",
            fDigitalOutput ? 1U : 0U);
    }
}

IOReturn YMFPCIAudioEngine::shutdownAction(OSObject *owner, void *, void *, void *, void *)
{
    static_cast<YMFPCIAudioEngine *>(owner)->shutdownLocked();
    return kIOReturnSuccess;
}

void YMFPCIAudioEngine::shutdownLocked()
{
    if (fDevice) {
        if (fLifecycleLock)
            IOLockLock(fLifecycleLock);
        IOLog("YMFPCI: engine shutdown entered digital=%u running=%u\n",
            fDigitalOutput ? 1U : 0U, fRunning ? 1U : 0U);
        fPlaybackActive = false;
        fCaptureActive = false;
        const IOReturn stopResult = stopDSP();
        IOLog("YMFPCI: engine shutdown DSP stop returned digital=%u result=%08x\n",
            fDigitalOutput ? 1U : 0U, static_cast<unsigned>(stopResult));
        fDevice->fHardwareIO.write16(fDevice->fHardwareIO.context,
            YMF_REG_SPDIFOUTCTRL, 0);
        fReady = false;
        if (fDevice->fConfiguredAudioEngine == this)
            fDevice->fConfiguredAudioEngine =
                fDevice->fAudioEngine != this ? fDevice->fAudioEngine : 0;
        const IOInterruptState state = IOSimpleLockLockDisableInterrupt(fLock);
        fDevice = 0;
        IOSimpleLockUnlockEnableInterrupt(fLock, state);
        if (fLifecycleLock)
            IOLockUnlock(fLifecycleLock);
        IOLog("YMFPCI: engine shutdown complete digital=%u\n",
            fDigitalOutput ? 1U : 0U);
    }
}

void YMFPCIAudioEngine::stop(IOService *provider)
{
    shutdown();
    super::stop(provider);
}

void YMFPCIAudioEngine::free()
{
    shutdown();
    releaseDMA(fCaptureSamples);
    releaseDMA(fSamples);
    releaseDMA(fControl);
    if (fLock)
        IOSimpleLockFree(fLock);
    if (fLifecycleLock)
        IOLockFree(fLifecycleLock);
    super::free();
}

bool YMFPCIMicEngine::init(OSDictionary *properties)
{
    fOwner = 0;
    fInputStream = 0;
    fReady = false;
    fRunning = false;
    fLastFrame = 0;
    return super::init(properties);
}

bool YMFPCIMicEngine::start(IOService *provider)
{
    if (!super::start(provider))
        return false;
    if (!fReady) {
        shutdown();
        super::stop(provider);
        return false;
    }
    return true;
}

bool YMFPCIMicEngine::initHardware(IOService *provider)
{
    if (!super::initHardware(provider))
        return false;
    YMFPCIAudioDevice *device = OSDynamicCast(YMFPCIAudioDevice, provider);
    if (!device || !device->fAudioEngine ||
        !device->fAudioEngine->fReady ||
        !device->fAudioEngine->fCaptureSamples.bytes)
        return false;
    fOwner = device->fAudioEngine;

    setDescription("Yamaha YMF754 Microphone");
    IOAudioSampleRate rate = { YMF_PCM_RATE, 0 };
    setSampleRate(&rate);
    setNumSampleFramesPerBuffer(YMF_PCM_FRAMES);
    setInputSampleOffset(256);

    IOAudioStreamFormat format;
    bzero(&format, sizeof(format));
    format.fNumChannels = 2;
    format.fSampleFormat = kIOAudioStreamSampleFormatLinearPCM;
    format.fNumericRepresentation = kIOAudioStreamNumericRepresentationSignedInt;
    format.fBitDepth = format.fBitWidth = 16;
    format.fAlignment = kIOAudioStreamAlignmentLowByte;
    format.fByteOrder = kIOAudioStreamByteOrderLittleEndian;
    format.fIsMixable = 1;

    IOAudioStream *input = new IOAudioStream;
    if (!input)
        return false;
    bool ok = input->initWithAudioEngine(this, kIOAudioStreamDirectionInput,
        1, "YMF754 microphone");
    if (ok) {
        input->setSampleBuffer(fOwner->fCaptureSamples.bytes, YMF_PCM_BYTES);
        input->addAvailableFormat(&format, &rate, &rate);
        ok = input->setFormat(&format, false) == kIOReturnSuccess &&
            addAudioStream(input) == kIOReturnSuccess;
    }
    if (ok)
        fInputStream = input;
    input->release();
    if (!ok)
        return false;

    IOAudioLevelControl *micVolume = IOAudioLevelControl::createVolumeControl(
        fOwner->fMicVolumeValue, YMF_AC97_MIC_VOLUME_MIN,
        YMF_AC97_MIC_VOLUME_MAX, static_cast<IOFixed>(-1736704),
        static_cast<IOFixed>(1310720), kIOAudioControlChannelIDAll, 0, 0,
        kIOAudioControlUsageInput);
    if (!micVolume)
        return false;
    micVolume->setValueChangeHandler(
        YMFPCIAudioEngine::micVolumeValueChanged, fOwner);
    const IOReturn controlResult = addDefaultAudioControl(micVolume);
    micVolume->release();
    if (controlResult != kIOReturnSuccess)
        return false;

    fReady = true;
    IOLog("YMFPCI: separate microphone engine registered; shared capture DMA=%08x\n",
        fOwner->fCaptureSamples.address);
    return true;
}

UInt32 YMFPCIMicEngine::getCurrentSampleFrame()
{
    return fOwner ? fOwner->currentCaptureFrame() : fLastFrame;
}

IOReturn YMFPCIMicEngine::performAudioEngineStart()
{
    if (!fReady || !fOwner)
        return kIOReturnNotReady;
    if (fRunning)
        return kIOReturnSuccess;
    const IOReturn result = fOwner->startCaptureEngine();
    if (result == kIOReturnSuccess) {
        fRunning = true;
        fLastFrame = getCurrentSampleFrame();
        takeTimeStamp(false);
        IOLog("YMFPCI: microphone engine started independently of playback\n");
    }
    return result;
}

IOReturn YMFPCIMicEngine::performAudioEngineStop()
{
    if (!fRunning) {
        if (fOwner)
            return fOwner->stopCaptureEngine();
        return kIOReturnSuccess;
    }
    fRunning = false;
    fLastFrame = 0;
    return fOwner ? fOwner->stopCaptureEngine() : kIOReturnSuccess;
}

IOReturn YMFPCIMicEngine::performFormatChange(IOAudioStream *,
    const IOAudioStreamFormat *format, const IOAudioSampleRate *rate)
{
    return supportedFormat(format) &&
        (!rate || (rate->whole == YMF_PCM_RATE && rate->fraction == 0)) ?
        kIOReturnSuccess : kIOReturnUnsupported;
}

IOReturn YMFPCIMicEngine::convertInputSamples(const void *samples, void *mix,
    UInt32 firstFrame, UInt32 frames, const IOAudioStreamFormat *format,
    IOAudioStream *stream)
{
    if (!fOwner || stream != fInputStream)
        return kIOReturnBadArgument;
    return fOwner->convertInputSamples(samples, mix, firstFrame, frames,
        format, stream);
}

void YMFPCIMicEngine::updateTimestamp()
{
    if (!fRunning)
        return;
    const UInt32 frame = getCurrentSampleFrame();
    if (frame < fLastFrame)
        takeTimeStamp();
    fLastFrame = frame;
}

void YMFPCIMicEngine::shutdown()
{
    IOCommandGate *gate = getCommandGate();
    if (gate)
        gate->runAction(shutdownAction);
    else
        shutdownLocked();
}

IOReturn YMFPCIMicEngine::shutdownAction(OSObject *owner, void *, void *, void *, void *)
{
    static_cast<YMFPCIMicEngine *>(owner)->shutdownLocked();
    return kIOReturnSuccess;
}

void YMFPCIMicEngine::shutdownLocked()
{
    if (fRunning)
        performAudioEngineStop();
    fReady = false;
}

void YMFPCIMicEngine::stop(IOService *provider)
{
    shutdown();
    super::stop(provider);
}

void YMFPCIMicEngine::free()
{
    shutdown();
    fInputStream = 0;
    fOwner = 0;
    super::free();
}
