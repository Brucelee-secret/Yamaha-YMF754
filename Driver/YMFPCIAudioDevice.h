/*
 * SPDX-License-Identifier: GPL-2.0-or-later
 * Initial Mac OS X IOKit scaffold for Yamaha YMF754 / DS-1E.
 */

#ifndef YMFPCI_AUDIO_DEVICE_H
#define YMFPCI_AUDIO_DEVICE_H

#include <IOKit/audio/IOAudioDevice.h>
#include <IOKit/pci/IOPCIDevice.h>
#include <IOKit/IOFilterInterruptEventSource.h>
#include <IOKit/IOInterruptEventSource.h>
#include "YMFPCIHardware.h"

class IOMemoryMap;
class YMFPCIAudioEngine;
class YMFPCIMicEngine;

class YMFPCIAudioDevice : public IOAudioDevice
{
    OSDeclareDefaultStructors(YMFPCIAudioDevice)

public:
    virtual bool init(OSDictionary *properties = 0);
    virtual bool initHardware(IOService *provider);
    virtual void stop(IOService *provider);
    virtual void free();
    virtual IOReturn performPowerStateChange(IOAudioDevicePowerState oldState,
        IOAudioDevicePowerState newState, UInt32 *microsecondsUntilComplete);

private:
    friend class YMFPCIAudioEngine;
    friend class YMFPCIMicEngine;
    YMFPCIAudioEngine *fAudioEngine;
    YMFPCIAudioEngine *fDigitalAudioEngine;
    YMFPCIMicEngine *fMicEngine;
    YMFPCIAudioEngine *fConfiguredAudioEngine;
    IOPCIDevice *fPCIDevice;
    IOMemoryMap *fRegisterMap;
    IOFilterInterruptEventSource *fInterruptSource;
    volatile UInt8 *fRegisters;
    bool fProviderOpen;
    bool fInterruptSourceAdded;
    bool fIOConfigured;
    bool fIOWasEnabled;
    bool fMemoryConfigured;
    bool fMemoryWasEnabled;
    bool fHardwareTouched;
    YMFPCIHardwareIO fHardwareIO;

    void releaseHardware();
    IOReturn armInterruptSource();
    void disarmInterruptSource();
    void releaseInterruptSource();
    void logAC97Snapshot(const char *stage);
    static bool interruptFilter(OSObject *owner,
        IOFilterInterruptEventSource *source);
    static void interruptAction(OSObject *owner,
        IOInterruptEventSource *source, int count);
    static YMFWord read16(void *context, unsigned offset);
    static YMFDword read32(void *context, unsigned offset);
    static void write16(void *context, unsigned offset, YMFWord value);
    static void write32(void *context, unsigned offset, YMFDword value);
    static YMFByte configRead8(void *context, unsigned offset);
    static void configWrite8(void *context, unsigned offset, YMFByte value);
    static void configWrite16(void *context, unsigned offset, YMFWord value);
    static void delayMicroseconds(void *context, unsigned value);
    static void sleepMilliseconds(void *context, unsigned value);
};

#endif /* YMFPCI_AUDIO_DEVICE_H */
