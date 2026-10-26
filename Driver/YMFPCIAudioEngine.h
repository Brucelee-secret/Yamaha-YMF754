/*
 * SPDX-License-Identifier: GPL-2.0-or-later
 * Native IOAudioFamily stereo playback adapter.
 */
#ifndef YMFPCI_AUDIO_ENGINE_H
#define YMFPCI_AUDIO_ENGINE_H

#include <IOKit/audio/IOAudioEngine.h>
#include <IOKit/audio/IOAudioControl.h>
#include <IOKit/IOBufferMemoryDescriptor.h>
#include <IOKit/IODMACommand.h>
#include <IOKit/IOFilterInterruptEventSource.h>
#include <IOKit/IOLocks.h>
#include "YMFPCIPCM.h"

class YMFPCIAudioDevice;
class YMFPCIMicEngine;

struct YMFDMABuffer {
    IOBufferMemoryDescriptor *memory;
    IODMACommand *command;
    void *bytes;
    UInt32 address;
    bool prepared;
};

class YMFPCIAudioEngine : public IOAudioEngine
{
    OSDeclareDefaultStructors(YMFPCIAudioEngine)
public:
    virtual bool init(OSDictionary *properties = 0);
    virtual bool start(IOService *provider);
    virtual bool initHardware(IOService *provider);
    virtual void stop(IOService *provider);
    virtual void free();
    virtual UInt32 getCurrentSampleFrame();
    virtual IOReturn performAudioEngineStart();
    virtual IOReturn performAudioEngineStop();
    virtual IOReturn performFormatChange(IOAudioStream *stream,
        const IOAudioStreamFormat *format, const IOAudioSampleRate *rate);
    virtual IOReturn clipOutputSamples(const void *mix, void *samples,
        UInt32 firstFrame, UInt32 frames, const IOAudioStreamFormat *format,
        IOAudioStream *stream);
    virtual IOReturn convertInputSamples(const void *samples, void *mix,
        UInt32 firstFrame, UInt32 frames, const IOAudioStreamFormat *format,
        IOAudioStream *stream);

    void setDigitalOutput(bool digital);
    void shutdown();
    void suspendHardware();
    bool resumeHardware();
    UInt32 currentCaptureFrame();
    IOReturn startCaptureEngine();
    IOReturn stopCaptureEngine();

private:
    friend class YMFPCIMicEngine;
    friend class YMFPCIAudioDevice;
    YMFPCIAudioDevice *fDevice;
    YMFPCIMicEngine *fMicEngine;
    YMFPCMLayout fLayout;
    YMFDMABuffer fControl, fSamples, fCaptureSamples;
    IOSimpleLock *fLock;
    IOLock *fLifecycleLock;
    bool fReady, fRunning, fSuspended, fCaptureRunning;
    bool fPlaybackActive, fCaptureActive;
    bool fOutputMuted, fDigitalOutput, fThreeDWide, fThreeDWideSupported;
    bool fReportedIRQ;
    bool fReportedProgress, fReportedWrap, fReportedZeroFrame, fReportedPCMData;
    bool fReportedOtherIRQ;
    bool fReportedDSPState;
    UInt32 fLastFrame, fPendingDiagnostics, fDiagnosticFrames;
    UInt32 fCaptureDiagnosticFrames, fCaptureDiagnosticNonzero;
    UInt32 fCaptureDiagnosticPeak;
    UInt32 fPendingIRQStatus;
    UInt16 fPendingINTFlag;
    SInt32 fVolumeValue, fMicVolumeValue;

    bool discoverLayout(YMFPCMLayout &layout);
    bool configureHardware();
    bool configureCodec();
    IOReturn startDSP();
    IOReturn stopDSP();
    void prepareCapturePath();
    void updateCaptureRouting();
    void initializePlaybackBanks();
    void shutdownLocked();
    static IOReturn shutdownAction(OSObject *owner, void *, void *, void *, void *);
    void initializeBanks();
    UInt32 currentFrameLocked();
    static bool allocateDMA(YMFDMABuffer &buffer, unsigned size);
    static void releaseDMA(YMFDMABuffer &buffer);
    static bool interruptFilter(OSObject *owner, IOFilterInterruptEventSource *source);
    static void interruptAction(OSObject *owner, IOInterruptEventSource *source, int count);
    static IOReturn volumeValueChanged(OSObject *target, IOAudioControl *control,
        SInt32 oldValue, SInt32 newValue);
    static IOReturn muteValueChanged(OSObject *target, IOAudioControl *control,
        SInt32 oldValue, SInt32 newValue);
    static IOReturn threeDWideValueChanged(OSObject *target,
        IOAudioControl *control, SInt32 oldValue, SInt32 newValue);
    static IOReturn micVolumeValueChanged(OSObject *target, IOAudioControl *control,
        SInt32 oldValue, SInt32 newValue);
};

class YMFPCIMicEngine : public IOAudioEngine
{
    OSDeclareDefaultStructors(YMFPCIMicEngine)
public:
    virtual bool init(OSDictionary *properties = 0);
    virtual bool start(IOService *provider);
    virtual bool initHardware(IOService *provider);
    virtual void stop(IOService *provider);
    virtual void free();
    virtual UInt32 getCurrentSampleFrame();
    virtual IOReturn performAudioEngineStart();
    virtual IOReturn performAudioEngineStop();
    virtual IOReturn performFormatChange(IOAudioStream *stream,
        const IOAudioStreamFormat *format, const IOAudioSampleRate *rate);
    virtual IOReturn convertInputSamples(const void *samples, void *mix,
        UInt32 firstFrame, UInt32 frames, const IOAudioStreamFormat *format,
        IOAudioStream *stream);

    void updateTimestamp();
    void shutdown();

private:
    friend class YMFPCIAudioEngine;
    static IOReturn shutdownAction(OSObject *owner, void *, void *, void *, void *);
    void shutdownLocked();
    YMFPCIAudioEngine *fOwner;
    IOAudioStream *fInputStream;
    bool fReady, fRunning;
    UInt32 fLastFrame;
};
#endif
